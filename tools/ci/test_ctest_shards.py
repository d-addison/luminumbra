#!/usr/bin/env python3
"""Offline regressions for shared CTest partitions and aggregate evidence."""

import argparse
import importlib.util
import io
import copy
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("ctest_shards", Path(__file__).with_name("ctest_shards.py"))
shards = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(shards)

COST_HINTS = {name: 830 for name in shards.HEAVY_NAMES} | {
    "FarLodWorker.CameraRegionCoveragePreservesLiveTerrainAndWaterOwnership": 180,
    "FarWorldCaveSelection.ProductionFacadeAdjacentTilesShareBordersHalosAndVerticalBrickSamples": 125,
    "WaterDeterminism.WaterResolutionMigrationConvergesAtLoadBothWays": 120,
}


def test(name, **properties):
    return {"name": name, "command": ["test-binary", name], "properties": [{"name": key, "value": value} for key, value in properties.items()]}


class CTestShardTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        environment = patch.dict(os.environ)
        environment.start()
        self.addCleanup(environment.stop)
        os.environ.pop("GITHUB_OUTPUT", None)
        self.tests = [test(f"Suite.Case{index}") for index in range(40)]
        self.names = [row["name"] for row in self.tests]

    def test_synthetic_plans_do_not_publish_workflow_outputs(self):
        self.assertNotIn("GITHUB_OUTPUT", os.environ)
        output = self.root / "step-output"
        sentinel = b"sentinel=retained\nprefab=true\n"
        for profile in ("asan", "gcc", "msvc", "ucrt64"):
            tests = [*self.tests, test("SimBudgetWorldParity.First"),
                     test("SimBudgetWorldParity.Second")]
            for shard in (shards.shard_ids(profile) if profile == "asan" else (0, 1, 2)):
                with self.subTest(profile=profile, shard=shard):
                    output.write_bytes(sentinel)
                    with patch.dict(os.environ, GITHUB_OUTPUT=str(output)):
                        self.write_plan(self.root, shard, tests=tests, profile=profile)
                    self.assertEqual(output.read_bytes(), sentinel)

    def test_cli_publishes_only_the_selected_real_plan(self):
        tests = self.policy_roster() + [test("PrefabRuntimeTest.Installed"), test("PrefabDigest.Content")]
        output = self.root / "step-output"
        sentinel = b"sentinel=retained\n"
        for script, profile in (("asan_shards.py", "asan"), ("ctest_shards.py", "asan"),
                                ("ctest_shards.py", "gcc"), ("ctest_shards.py", "msvc"),
                                ("ctest_shards.py", "ucrt64")):
            for shard in (shards.shard_ids(profile) if profile == "asan" else (0, 1, 2)):
                with self.subTest(script=script, profile=profile, shard=shard):
                    manifest = self.write_plan(self.root, shard, tests=tests, profile=profile)
                    selected = manifest["partitions"][0 if shard == 0 else shard - 1]
                    expected = any(name.startswith("PrefabRuntimeTest.") for name in selected)
                    output.write_bytes(sentinel)
                    result = subprocess.run(
                        [sys.executable, "-I", "-B", str(Path(__file__).with_name(script)), "plan",
                         "--directory", str(self.root), "--profile", profile, "--shard", str(shard)],
                        env=dict(os.environ, GITHUB_OUTPUT=str(output)),
                        capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    published = f"prefab={str(expected).lower()}{os.linesep}".encode("utf-8")
                    self.assertEqual(output.read_bytes(), sentinel + published)

    def test_full_capture_prefab_output_survives_both_contract_suites(self):
        tests = self.policy_roster() + [test("PrefabRuntimeTest.Installed"), test("PrefabDigest.Content")]
        self.write_plan(self.root, 0, tests=tests)
        output = self.root / "step-output"
        output.write_bytes(b"sentinel=retained\n")
        environment = dict(os.environ, GITHUB_OUTPUT=str(output))
        result = subprocess.run(
            [sys.executable, "-I", "-B", str(Path(__file__).with_name("ctest_shards.py")),
             "plan", "--directory", str(self.root), "--shard", "0", "--require-suite", "PrefabRuntimeTest."],
            env=environment, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        expected = b"sentinel=retained\n" + f"prefab=true{os.linesep}".encode("utf-8")
        self.assertEqual(output.read_bytes(), expected)
        for script, case in (("test_asan_shards.py", "AsanShardTests"),
                             ("test_ctest_shards.py", "CTestShardTests")):
            with self.subTest(script=script):
                result = subprocess.run(
                    [sys.executable, "-I", "-B", str(Path(__file__).with_name(script)),
                     case + ".test_synthetic_plans_do_not_publish_workflow_outputs"],
                    env=environment, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(output.read_bytes(), expected)

    def test_cli_output_absent_empty_unwritable_and_invalid_inventory(self):
        output = self.root / "step-output"
        command = [sys.executable, "-I", "-B", str(Path(__file__).with_name("ctest_shards.py")),
                   "plan", "--directory", str(self.root), "--shard", "0"]
        self.write_plan(self.root, 0, tests=self.policy_roster())
        for value in (None, "", str(self.root)):
            environment = dict(os.environ)
            if value is not None:
                environment["GITHUB_OUTPUT"] = value
            with self.subTest(output=value):
                result = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 1 if value else 0, result.stdout + result.stderr)
                self.assertFalse(output.exists())
                if value:
                    self.assertIn("CTest plan refused:", result.stderr)
        sentinel = b"sentinel=retained\nprefab=true\n"
        for document in ("{", '{"tests": []}', '{"tests": [{"name": "unknown"}]}', None):
            with self.subTest(document=document):
                inventory = self.root / "ctest.inventory.json"
                if document is None:
                    inventory.unlink()
                else:
                    inventory.write_text(document, encoding="utf-8")
                output.write_bytes(sentinel)
                result = subprocess.run(command, env=dict(os.environ, GITHUB_OUTPUT=str(output)),
                                        capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn("CTest plan refused:", result.stderr)
                self.assertEqual(output.read_bytes(), sentinel)

    def test_policy_cli_publication_and_resolved_execution_preserve_fixture_contract(self):
        output = self.root / "step-output"
        sentinel = b"sentinel=retained\nprefab=true\n"
        resolved = self.root.resolve()

        class WindowsBuildAlias(type(self.root)):
            def resolve(self):
                return resolved

        alias = WindowsBuildAlias("C:/Users/RUNNER~1/AppData/Local/Temp/build")
        for profile in shards.PROFILES:
            CaseProgressResult._phase(profile, "all", 0, "fixture-start")
            tests = self.policy_roster(profile) + [
                test("PrefabRuntimeTest.Installed", FIXTURES_REQUIRED=["runtime"], TIMEOUT=30),
                test("PrefabDigest.Content", FIXTURES_SETUP=["runtime"], TIMEOUT=30),
                test("Runtime.Cleanup", FIXTURES_CLEANUP=["runtime"], TIMEOUT=30)]
            # At most max(FLOORS.values()) + 3 rows; retain the original
            # recursive construction once per profile as an exact byte oracle.
            expected_current = copy.deepcopy(tests)
            for row in expected_current:
                row["properties"].reverse()
            expected_discovery = json.dumps({"tests": expected_current})
            expected_tests = [{**row, "properties": list(reversed(row["properties"]))}
                              for row in expected_current]
            encode = json.dumps
            preparations = 0
            current = None

            def encode_discovery(document, *args, **kwargs):
                nonlocal preparations
                if isinstance(document, dict) and "tests" in document and document["tests"] is current:
                    preparations += 1
                return encode(document, *args, **kwargs)

            with patch.object(json, "dumps", new=encode_discovery):
                # One bounded preparation per profile; nested values stay read-only.
                with patch.object(copy, "deepcopy", side_effect=AssertionError(
                        "execution discovery fixture must not recursively copy each lane")):
                    current = [{**row, "properties": list(reversed(row["properties"]))} for row in tests]
                    discovery = json.dumps({"tests": current})
                CaseProgressResult._phase(profile, "all", 0, "fixture-finish")
                for selection in ("full", "light", "heavy"):
                    if profile == "asan" and selection == "heavy":
                        for shard in shards.shard_ids(profile):
                            before = {name: (self.root / name).read_bytes() for name in
                                      ("ctest.partition.json", "ctest.shard-index.txt", "step-output")}
                            with self.assertRaisesRegex(ValueError, "empty CTest shard"):
                                self.write_plan(self.root, shard, tests=tests, profile=profile, selection=selection)
                            self.assertEqual({name: (self.root / name).read_bytes() for name in before}, before)
                        continue
                    for shard in shards.shard_ids(profile):
                        with self.subTest(profile=profile, selection=selection, shard=shard):
                            CaseProgressResult._phase(profile, selection, shard, "lane-fixture-start")
                            output.write_bytes(sentinel)
                            CaseProgressResult._phase(profile, selection, shard, "lane-fixture-finish")
                            CaseProgressResult._phase(profile, selection, shard, "plan-start")
                            with patch.dict(os.environ, GITHUB_OUTPUT=str(output)):
                                self.write_plan(self.root, shard, tests=tests, profile=profile,
                                                selection=selection)
                            CaseProgressResult._phase(profile, selection, shard, "plan-finish")
                            self.assertEqual(output.read_bytes(), sentinel)
                            CaseProgressResult._phase(profile, selection, shard, "positive-start")
                            result = subprocess.run(
                                [sys.executable, "-I", "-B", str(Path(__file__).with_name("ctest_shards.py")),
                                 "plan", "--directory", str(self.root), "--profile", profile,
                                 "--shard", str(shard), "--selection", selection],
                                env=dict(os.environ, GITHUB_OUTPUT=str(output)),
                                capture_output=True, text=True, timeout=10)
                            CaseProgressResult._phase(profile, selection, shard, "positive-finish")
                            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                            manifest = shards.read_json(self.root / "ctest.partition.json")
                            self.assertEqual(manifest["selection"], selection)
                            shards.validate_union(shards.select_tests(tests, profile, selection),
                                                  manifest["partitions"])
                            selected = manifest["partitions"][shard - 1]
                            prefab = "PrefabRuntimeTest.Installed" in selected
                            self.assertEqual(output.read_bytes(), sentinel +
                                             f"prefab={str(prefab).lower()}{os.linesep}".encode("utf-8"))
                            fixture = {"PrefabRuntimeTest.Installed", "PrefabDigest.Content", "Runtime.Cleanup"}
                            self.assertIn(fixture.intersection(selected), (set(), fixture))
                            if selection == "heavy":
                                self.assertEqual(set().union(*map(set, manifest["partitions"])), shards.HEAVY_NAMES)
                                self.assertFalse(prefab)
                            CaseProgressResult._phase(profile, selection, shard, "run-fixture-start")
                            response = subprocess.CompletedProcess([], 0, stdout=discovery)
                            self.assertEqual(response.stdout, expected_discovery)
                            CaseProgressResult._phase(profile, selection, shard, "run-fixture-finish")
                            CaseProgressResult._phase(profile, selection, shard, "resolved-start")
                            with patch.object(shards.subprocess, "run", side_effect=[
                                    response, subprocess.CompletedProcess([], 8)]) as invoke:
                                self.assertEqual(shards.run(alias, profile), 8)
                            CaseProgressResult._phase(profile, selection, shard, "resolved-finish")
                            self.assertEqual(invoke.call_args.args[0][4], str(resolved))
                            self.assertIn(str(resolved / "ctest.shard-index.txt"), invoke.call_args.args[0])
                            CaseProgressResult._phase(profile, selection, shard, "refusal-fixture-start")
                            before = {name: (self.root / name).read_bytes() for name in
                                      ("ctest.partition.json", "ctest.shard-index.txt", "step-output")}
                            # A partial discovery must refuse before changing plan or workflow outputs.
                            (self.root / "ctest.inventory.json").write_text("{", encoding="utf-8")
                            CaseProgressResult._phase(profile, selection, shard, "refusal-fixture-finish")
                            CaseProgressResult._phase(profile, selection, shard, "negative-start")
                            result = subprocess.run(
                                [sys.executable, "-I", "-B", str(Path(__file__).with_name("ctest_shards.py")),
                                 "plan", "--directory", str(self.root), "--profile", profile,
                                 "--shard", str(shard), "--selection", selection],
                                env=dict(os.environ, GITHUB_OUTPUT=str(output)),
                                capture_output=True, text=True, timeout=10)
                            CaseProgressResult._phase(profile, selection, shard, "negative-finish")
                            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                            for name, content in before.items():
                                self.assertEqual((self.root / name).read_bytes(), content)
            self.assertEqual(preparations, 1)
            self.assertEqual(tests, expected_tests)

    def test_plan_fixture_encodes_shared_inputs_once_without_retaining_mutations(self):
        encode = json.dumps
        for shared in (True, False):
            with self.subTest(shared=shared):
                all_tests = self.tests if shared else copy.deepcopy(self.tests)
                expected = encode({"tests": self.tests}).encode("utf-8")
                with patch.object(json, "dumps", wraps=encode) as serialize:
                    self.write_plan(self.root, 1, all_tests=all_tests)
                calls = [call for call in serialize.call_args_list
                         if isinstance(call.args[0], dict) and "tests" in call.args[0]
                         and (call.args[0]["tests"] is self.tests or call.args[0]["tests"] is all_tests)]
                self.assertEqual(len(calls), 1 if shared else 2)
                self.assertEqual((self.root / "ctest.all.json").read_bytes(), expected)
                self.assertEqual((self.root / "ctest.inventory.json").read_bytes(), expected)
                self.assertEqual(encode({"tests": self.tests}).encode(), expected)
                self.assertEqual(all_tests, self.tests)
        self.tests[0]["command"].append("--changed")
        expected = encode({"tests": self.tests}).encode("utf-8")
        with patch.object(json, "dumps", wraps=encode) as serialize:
            self.write_plan(self.root, 1)
        calls = [call for call in serialize.call_args_list
                 if isinstance(call.args[0], dict) and call.args[0].get("tests") is self.tests]
        self.assertEqual(len(calls), 1)
        self.assertEqual((self.root / "ctest.all.json").read_bytes(), expected)
        self.assertEqual((self.root / "ctest.inventory.json").read_bytes(), expected)
        independent = copy.deepcopy(self.tests)
        independent[0]["command"].append("--independent")
        with self.assertRaisesRegex(ValueError, "execution inventory differs"):
            self.write_plan(self.root, 1, all_tests=independent)
        self.assertEqual((self.root / "ctest.all.json").read_bytes(),
                         encode({"tests": independent}).encode("utf-8"))
        self.assertEqual((self.root / "ctest.inventory.json").read_bytes(), expected)
        with self.assertRaisesRegex(ValueError, "empty CTest inventory"):
            self.write_plan(self.root, 1, tests=[])

    def write_plan(self, directory, shard, tests=None, all_tests=None, profile="gcc", roots=None, selection=None):
        directory.mkdir(parents=True, exist_ok=True)
        tests = self.tests if tests is None else tests
        all_tests = tests if all_tests is None else all_tests
        # Reuse only this call's encoding when both fixture inputs are the same object.
        # Both files and the real planner remain independent consumers.
        all_text = json.dumps({"tests": all_tests})
        (directory / "ctest.all.json").write_text(all_text, encoding="utf-8")
        eligible_text = all_text if tests is all_tests else json.dumps({"tests": tests})
        (directory / "ctest.inventory.json").write_text(eligible_text, encoding="utf-8")
        # Artifact directories model producers with identical hosted checkout paths.
        roots = shards.inventory_roots(self.root) if roots is None else roots
        with patch.object(shards, "inventory_roots", return_value=roots):
            shards.plan(directory, shard, profile, selection=selection)
        return shards.read_json(directory / "ctest.partition.json")

    def reports(self, full=False, profile="gcc"):
        for shard in ((0,) if full else shards.shard_ids(profile)):
            directory = self.root / f"shard-{shard}"
            manifest = self.write_plan(directory, shard, profile=profile)
            root = ET.Element("testsuite", failures="0", errors="0", skipped="0")
            for name in manifest["partitions"][0 if full else shard - 1]:
                case = ET.SubElement(root, "testcase", name=name, status="run")
                ET.SubElement(case, "system-out").text = "receipt: " + name
            ET.ElementTree(root).write(directory / "ctest.junit.xml")

    def merge(self, full=False, profile="gcc"):
        shards.merge(self.root, self.root / "merged.xml", full=full, profile=profile)

    def test_plan_manifest_bytes_match_general_evidence_in_every_profile(self):
        for profile in shards.PROFILES:
            tests = self.policy_roster(profile)
            tests[0] = test("PrefabRuntimeTest.Identity", TIMEOUT=12.5,
                            ENVIRONMENT=["DATA=résumé/世界", "ORDER=second"])
            tests[0]["backtrace"] = 7
            all_tests = copy.deepcopy(tests) + [test("Manual.Only", LABELS=["manual"])]
            all_tests[0]["backtrace"] = 99
            all_tests[0]["properties"].reverse()
            eligible = [row["name"] for row in tests]
            positions = {name: index for index, name in enumerate(eligible, 1)}
            # Snapshot each distinct input once; every plan still checks it.
            fixtures = []
            for timings, roots in (
                    (None, {"source": "/opt/source", "build": "/opt/build"}),
                    ({eligible[0]: 1.25, eligible[1]: 90.0},
                     {"source": "C:/source/世界", "build": "C:/build/release"})):
                fixtures.append((timings, roots, copy.deepcopy((tests, all_tests, roots, timings))))
            for name, rows in (("ctest.inventory.json", tests), ("ctest.all.json", all_tests)):
                (self.root / name).write_text(json.dumps({"tests": rows}), encoding="utf-8")
            for selection in (None, "full", "light", "heavy"):
                lanes = (*shards.shard_ids(profile), 0) if profile != "asan" and selection in (None, "full") else shards.shard_ids(profile)
                selected = shards.select_tests(tests, profile, selection)
                if profile == "asan" and selection == "heavy":
                    with self.assertRaisesRegex(ValueError, "empty CTest shard"):
                        shards._partition_validated(all_tests, selected, profile)
                    continue
                for timings, roots, inputs in fixtures:
                    # The general API remains the complete pre-reuse manifest oracle.
                    # Only full capture changes the partition shape between lanes.
                    parts_by_capture = {False: shards._partition_validated(all_tests, selected, profile, timings)}
                    if 0 in lanes:
                        parts_by_capture[True] = [selected]
                    evidence = {full: shards.evidence_for(tests, parts, profile, timings, roots, selection)
                                for full, parts in parts_by_capture.items()}
                    for shard in lanes:
                        with self.subTest(profile=profile, selection=selection, timings=timings, shard=shard):
                            parts = parts_by_capture[shard == 0]
                            expected = {**evidence[shard == 0], "shard": shard}
                            expected_bytes = (json.dumps(expected, sort_keys=True) + os.linesep).encode("utf-8")
                            names = parts[0 if shard == 0 else shard - 1]
                            indices = [positions[name] for name in names]
                            expected_indices = (",".join(map(str, [indices[0], indices[0], 1, *indices[1:]])) + os.linesep).encode()
                            with patch.object(shards, "inventory_roots", return_value=roots):
                                prefab = shards.plan(self.root, shard, profile, timings, selection)
                            self.assertEqual((self.root / "ctest.partition.json").read_bytes(), expected_bytes)
                            self.assertEqual((self.root / "ctest.shard-index.txt").read_bytes(), expected_indices)
                            self.assertEqual(prefab, any(name.startswith("PrefabRuntimeTest.") for name in names))
                            self.assertEqual((tests, all_tests, roots, timings), inputs)

    def test_manifest_oracle_construction_is_bounded_with_real_plans(self):
        # Count outer snapshot constructions, not deepcopy's recursive visits.
        original_copy = copy.deepcopy
        original_plan = shards.plan
        original_partition = shards._partition_validated
        original_evidence = shards.evidence_for
        counts = dict(snapshots=0, partitions=0, evidence=0, plans=0)
        copying = 0
        planning = False

        def snapshot(*args, **kwargs):
            nonlocal copying
            if not copying:
                counts["snapshots"] += 1
            copying += 1
            try:
                return original_copy(*args, **kwargs)
            finally:
                copying -= 1

        def plan(*args, **kwargs):
            nonlocal planning
            counts["plans"] += 1
            planning = True
            try:
                return original_plan(*args, **kwargs)
            finally:
                planning = False

        def partition(*args, **kwargs):
            if not planning:
                counts["partitions"] += 1
            return original_partition(*args, **kwargs)

        def evidence(*args, **kwargs):
            if not planning:
                counts["evidence"] += 1
            return original_evidence(*args, **kwargs)

        with patch.object(copy, "deepcopy", new=snapshot), patch.object(
                shards, "plan", side_effect=plan), patch.object(
                shards, "_partition_validated", side_effect=partition), patch.object(
                shards, "evidence_for", side_effect=evidence), patch.object(
                self, "policy_roster", wraps=self.policy_roster) as roster, patch.object(
                shards, "PROFILES", {"gcc": shards.PROFILES["gcc"]}):
            # Use the full-size GCC fixture for this additional cost check;
            # the original test separately retains all 76 plans/four profiles.
            self.test_plan_manifest_bytes_match_general_evidence_in_every_profile()
        self.assertEqual(roster.call_count, 1)
        # One independent all-test copy plus two before-plan snapshots;
        # eight partitioned combinations and four full-capture oracles.
        self.assertEqual(counts, dict(snapshots=3, partitions=8, evidence=12, plans=20))

    def test_plan_normalizes_and_hashes_each_independent_inventory_once(self):
        original_normalize = shards.execution_inventory
        original_digest = shards.digest
        for profile in shards.PROFILES:
            tests = copy.deepcopy(self.tests)
            tests[0]["properties"] = [{"name": "TIMEOUT", "value": 2.5},
                                      {"name": "ENVIRONMENT", "value": ["A=one", "B=two"]}]
            all_tests = copy.deepcopy(tests)
            all_tests[0]["properties"].reverse()
            all_tests[0]["backtrace"] = 11
            for name, rows in (("ctest.inventory.json", tests), ("ctest.all.json", all_tests)):
                (self.root / name).write_text(json.dumps({"tests": rows}), encoding="utf-8")
            roots = shards.inventory_roots(self.root)
            timings = {self.names[0]: 4.0}
            parts = shards._partition_validated(all_tests, self.names, profile, timings)
            expected = {**shards.evidence_for(tests, parts, profile, timings, roots), "shard": 1}
            normalized, contracts, partitions = [], [], []

            def normalize(rows):
                result = original_normalize(rows)
                normalized.append((rows, copy.deepcopy(rows), result))
                return result

            def digest(value):
                result = original_digest(value)
                if isinstance(value, dict) and set(value) == {"roots", "tests"}:
                    contracts.append((value, result))
                if isinstance(value, dict) and set(value) == {"selection", "partitions"}:
                    partitions.append(value)
                return result

            with self.subTest(profile=profile), patch.object(shards, "execution_inventory", side_effect=normalize), patch.object(
                    shards, "digest", side_effect=digest):
                shards.plan(self.root, 1, profile, timings)
                self.assertEqual((self.root / "ctest.partition.json").read_bytes(),
                                 (json.dumps(expected, sort_keys=True) + os.linesep).encode("utf-8"))
                # Count real normalization/hash calls only after checking complete output.
                self.assertEqual(len(normalized), 2)
                self.assertEqual(len(contracts), 2)
                self.assertIsNot(normalized[0][0], normalized[1][0])
                self.assertEqual(normalized[0][0], tests)
                self.assertEqual(normalized[1][0], all_tests)
                for index, (rows, before, result) in enumerate(normalized):
                    self.assertEqual(rows, before)
                    self.assertIs(contracts[index][0]["tests"], result)
                    self.assertEqual(contracts[index][0]["roots"], roots)
                    self.assertEqual(contracts[index][1], expected["inventory_digest"])
                self.assertEqual(len(partitions), 3)
                self.assertEqual(partitions[-1], {"selection": None, "partitions": parts})
                self.assertIsNot(partitions[-1]["partitions"], partitions[0]["partitions"])

    def test_cli_independent_discovery_refuses_before_any_plan_or_output_write(self):
        script = str(Path(__file__).with_name("ctest_shards.py"))
        for profile in shards.PROFILES:
            tests = self.policy_roster(profile)
            for change in ("command", "non-finite"):
                changed = copy.deepcopy(tests)
                if change == "command":
                    changed[0]["command"].append("--different-execution")
                else:
                    changed[0]["properties"].append({"name": "TIMEOUT", "value": float("nan")})
                for existing in (False, True):
                    with self.subTest(profile=profile, change=change, existing=existing):
                        directory = self.root / f"{profile}-{change}-{existing}"
                        directory.mkdir()
                        for name, rows in (("ctest.inventory.json", tests), ("ctest.all.json", changed)):
                            (directory / name).write_text(json.dumps({"tests": rows}), encoding="utf-8")
                        outputs = [directory / name for name in
                                   ("ctest.partition.json", "ctest.shard-index.txt", "workflow-output")]
                        if existing:
                            for path in outputs:
                                path.write_bytes(b"existing bytes retained\n")
                        before = {path.name: path.read_bytes() for path in directory.iterdir()}
                        result = subprocess.run([sys.executable, "-I", "-B", script, "plan", "--directory", str(directory),
                                                 "--profile", profile, "--shard", "1"],
                                                env=dict(os.environ, GITHUB_OUTPUT=str(outputs[-1])),
                                                capture_output=True, text=True, timeout=10)
                        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                        self.assertIn("execution inventory differs" if change == "command" else "non-finite", result.stderr)
                        self.assertEqual(result.stdout, "")
                        self.assertEqual({path.name: path.read_bytes() for path in directory.iterdir()}, before)

    def test_exact_union_stable_hash_and_registration_order_independence(self):
        parts = shards.partition(self.tests, self.names)
        shards.validate_union(self.names, parts)
        self.assertEqual(shards.partition(self.tests, self.names), parts)
        reordered = shards.partition(list(reversed(self.tests)), list(reversed(self.names)))
        self.assertEqual([set(part) for part in parts], [set(part) for part in reordered])
        self.assertTrue(all(part == [name for name in self.names if name in part] for part in parts))

    def test_union_rejects_overlap_omission_duplicates_and_unknown(self):
        for parts in ((self.names, self.names[:1]), (self.names[:-1], []),
                      (self.names[:20] + self.names[:1], self.names[20:]),
                      (self.names[:20], [*self.names[20:], "unknown"]),
                      (self.names[:20], self.names[21:])):
            with self.subTest(parts=parts), self.assertRaises(ValueError):
                shards.validate_union(self.names, [list(part) for part in parts])

    def test_unknown_timings_have_conservative_nonzero_weight(self):
        result = shards.partition(self.tests, self.names, timings={self.names[0]: 0.1})
        shards.validate_union(self.names, result)
        self.assertLessEqual(abs(len(result[0]) - len(result[1])), 2)
        self.assertEqual(result, shards.partition(self.tests, self.names, timings={self.names[0]: 0.1}))
        for value in (0, -1, float("nan"), float("inf"), True, "1"):
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "invalid timings"):
                shards.partition(self.tests, self.names, timings={self.names[0]: value})

    def test_fixture_dependency_and_lock_closure_is_transitive(self):
        related = [test("setup", FIXTURES_SETUP=["art"]),
                   test("consumer", FIXTURES_REQUIRED=["art"], DEPENDS=["dependency"]),
                   test("cleanup", FIXTURES_CLEANUP=["art"]),
                   test("dependency", RESOURCE_LOCK=["shared-output"]),
                   test("writer", RESOURCE_LOCK=["shared-output"], RUN_SERIAL=True)]
        tests = self.tests + related
        names = [row["name"] for row in tests]
        parts = shards.partition(tests, names)
        owners = [part for part in parts if "setup" in part]
        self.assertEqual(len(owners), 1)
        self.assertTrue({row["name"] for row in related} <= set(owners[0]))
        for removed in related:
            with self.subTest(removed=removed), self.assertRaisesRegex(ValueError, "closure"):
                shards.partition(tests, [name for name in names if name != removed["name"]])
        self.assertTrue(related[-1]["properties"][-1]["value"])

    def contract_registration_inventory(self, windows, standalone=False, extra=""):
        source = self.root / ("windows" if windows else "posix") / ("local" if standalone else "engine")
        source.mkdir(parents=True)
        # Execute the shipping Python registration tail in a compiler-free
        # project. The outer conditional replaces BUILD_TESTING; no targets,
        # compiler checks, dependency downloads or native tests are involved.
        if standalone:
            local = ROOT / "tools/ci/local_check_contracts.cmake"
            if extra:
                local_source = local.read_text(encoding="utf-8").replace(
                    '${CMAKE_CURRENT_LIST_DIR}', local.parent.as_posix())
                self.assertEqual(local_source.count("if(WIN32)\n"), 1)
                local = source / "local_check_contracts.cmake"
                local.write_text(local_source.replace("if(WIN32)\n", extra + "if(WIN32)\n"),
                                 encoding="utf-8")
            registrations = 'include("' + local.as_posix() + '")\n'
        else:
            cmake = (ROOT / "tools/CMakeLists.txt").read_text(encoding="utf-8")
            marker = "    get_property(_tools_existing_tests DIRECTORY PROPERTY TESTS)\n"
            self.assertEqual(cmake.count(marker), 1)
            registrations = "if(TRUE)\n" + marker + cmake.split(marker, 1)[1]
            if extra:
                marker = "    if(WIN32)\n        # Both Windows shard profiles"
                self.assertIn(marker, registrations)
                registrations = registrations.replace(marker, extra + marker)
        (source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.20)\nproject(ContractScheduling LANGUAGES NONE)\n'
            'enable_testing()\nfind_package(Python3 COMPONENTS Interpreter REQUIRED)\n'
            f'set(WIN32 {"TRUE" if windows else "FALSE"})\n'
            f'set(UNIX {"FALSE" if windows else "TRUE"})\n'
            f'set(CMAKE_CURRENT_SOURCE_DIR "{(ROOT / "tools").as_posix()}")\n'
            'add_test(NAME ExistingCMakeContract COMMAND "${CMAKE_COMMAND}" -E true)\n'
            'set_tests_properties(ExistingCMakeContract PROPERTIES LABELS "ci;tools;net" TIMEOUT 30)\n'
            + registrations,
            encoding="utf-8")
        build = source / "build"
        configured = subprocess.run(["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja",
                        "--trace-expand", "--trace-format=json-v1",
                        f"-DPython3_EXECUTABLE={sys.executable}"],
                       check=True, capture_output=True, text=True, encoding="utf-8", timeout=10)
        cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
        self.assertNotRegex(cache, r"(?m)^CMAKE_(?:C|CXX)_COMPILER:")
        result = subprocess.run(["ctest", "--test-dir", str(build), "--show-only=json-v1"],
                                check=True, capture_output=True, text=True, encoding="utf-8", timeout=10)
        rows = shards.inventory(json.loads(result.stdout))
        declarations = [json.loads(line) for line in configured.stderr.splitlines() if line.startswith("{")]
        self.assert_registration_inventory(rows, declarations)
        return rows

    def assert_registration_inventory(self, rows, declarations):
        # Expanded CMake calls are independent of CTest's discovered properties.
        # Compare multisets so a missing or repeated registration cannot cancel out.
        registered = [call["args"][1] for call in declarations
                      if call.get("cmd") == "add_test" and call["args"][0] == "NAME"]
        self.assertCountEqual([row["name"] for row in rows], registered)
        expected = set()
        for call in declarations:
            if call.get("cmd") != "set_tests_properties":
                continue
            args = call["args"]
            boundary = args.index("PROPERTIES")
            properties = dict(zip(args[boundary + 1::2], args[boundary + 2::2]))
            if {"ci", "tools"} <= set(properties.get("LABELS", "").split(";")):
                expected.update(args[:boundary])
        selected = [row["name"] for row in rows if {"ci", "tools"} <= set(
            next((prop["value"] for prop in row["properties"] if prop["name"] == "LABELS"), []))]
        self.assertCountEqual(selected, expected)

    def contract_priorities(self):
        spec = importlib.util.spec_from_file_location("changed_tus_roster", ROOT / "tools/ci/test_changed_tus.py")
        changed = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(changed)
        return {"WorkflowPolicyContract": 30,
                **{"ChangedTranslationUnitsContract" + ("" if part == "0" else "Part" + part): 8
                   for part in changed.CHANGED_TUS_PARTITIONS},
                **{"CTestShardContractPart" + part: 25 for part in CTEST_SHARD_PARTITIONS},
                "WorkflowPolicyContractPart1": 21, "WorkflowPolicyContractPart8": 20,
                "WorkflowPolicyContractPart9": 19, "OfflineDependenciesContractPart2": 18,
                "WorkflowPolicyContractPart10": 17, "WorkflowPolicyContractPart11": 16,
                "WorkflowPolicyContractPart12": 15,
                "OfflineDependenciesContract": 10, "OfflineDependenciesContractPart1": 10}

    def test_windows_contract_reservations_cover_both_projects_and_preserve_shards(self):
        spec = importlib.util.spec_from_file_location("offline_roster", ROOT / "tools/ci/test_offline_deps.py")
        offline = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(offline)
        local_names = {name for name in self.contract_priorities()
                       if name.startswith("ChangedTranslationUnitsContract")} | {
            "OfflineDependenciesContract" + ("" if part == "0" else "Part" + part)
            for part in offline.OFFLINE_PARTITIONS}
        borrowed_input_names = {
            "RmlUiSourceScriptContract", "RmlUiSourceCommandContract",
            "LocalRmlUiRecipeContract", "LocalRmlUiRecoveryFailureContract",
            *{"RmlUiSourceFixture_" + name for name in (
                "every_changed_file_preserves_operations_defaults_and_enum_declarations",
                "files_mixed_missing_extra_and_partial_refuse_untouched",
                "git_pin_index_modes_flags_alternates_and_config_refusals",
                "mode_and_unreadable_refuse", "nonordinary_files_and_links",
                "operation_assertions_use_strict_utf8_with_a_cp1252_default")},
            *{"RmlUiSourceInterruption_" + name for name in
              ("check", "apply", "postverify", "rename", "receipt", "admission")},
            *{"LocalRmlUiRecipe_" + name for name in (
                "worker_configures_both_owned_disconnected_sources",
                "worker_refuses_invalid_gcc_receipts", "worker_refuses_invalid_tidy_receipts")},
            *{"RmlUiSourceRecovery_" + name for name in (
                "borrowed_archive_without_git_returns_its_original_metadata",
                "borrowed_recovery_refuses_unknown_partial_and_changed_metadata",
                "setup_final_verification_failure_returns_the_complete_input",
                "teardown_refusal_preserves_ancestor_and_can_retry_both_locations")},
            *{"LocalRmlUiRecovery_" + name for name in (
                "beforepatch", "afterpatch", "beforerename", "afterrename", "receipt", "configure")},
        }
        priorities = self.contract_priorities()
        for standalone in (False, True):
            inventories = {}
            for windows in (False, True):
                rows = self.contract_registration_inventory(windows, standalone)
                inventories[windows] = rows
                selected = []
                for row in rows:
                    props = {prop["name"]: prop["value"] for prop in row["properties"]}
                    python_contract = {"ci", "tools"} <= set(props.get("LABELS", [])) and any(
                        argument.endswith(".py") for argument in row["command"])
                    with self.subTest(windows=windows, standalone=standalone, name=row["name"]):
                        if python_contract:
                            selected.append(row["name"])
                            borrowed_input = row["name"] in borrowed_input_names
                            self.assertEqual(props["TIMEOUT"], 60 if borrowed_input else 30)
                            expected = 4 if windows else 2 if row["name"] == "WorkflowPolicyContractPart9" else 1
                            self.assertEqual(props.get("PROCESSORS", 1), expected)
                            self.assertEqual(props.get("COST", 0), priorities.get(row["name"], 0) if windows else 0)
                            for relationship in shards.RELATIONS:
                                if borrowed_input and relationship == "RESOURCE_LOCK":
                                    self.assertEqual(props[relationship], ["rmlui_offline_input"])
                                else:
                                    self.assertNotIn(relationship, props)
                        else:
                            self.assertNotIn("PROCESSORS", props)
                            self.assertNotIn("COST", props)
                self.assertTrue(local_names <= set(selected))
                if not standalone:
                    self.assertTrue(borrowed_input_names <= set(selected))
                    self.assertTrue({"ContainmentLinkClosureContract",
                                     "FirstPartyExceptionsContract"} <= set(selected))
                names = [row["name"] for row in rows]
                unthrottled = copy.deepcopy(rows)
                for row in unthrottled:
                    row["properties"] = [prop for prop in row["properties"]
                                         if prop["name"] not in {"PROCESSORS", "COST"}]
                for profile in ("gcc", "msvc", "ucrt64"):
                    # Reservations/costs affect only CTest's scheduler, never
                    # shard membership, index order or relationship closure.
                    for operation in (shards.partition, shards._partition_validated, shards._partition_projected):
                        self.assertEqual(operation(rows, names, profile), operation(unthrottled, names, profile))
            posix = {row["name"]: row for row in inventories[False]}
            for row in inventories[True]:
                if row["name"] not in selected:
                    continue
                original = posix[row["name"]]
                self.assertEqual(row["command"], original["command"])
                scheduling = {"PROCESSORS", "COST", "WORKING_DIRECTORY"}
                self.assertEqual([prop for prop in row["properties"] if prop["name"] not in scheduling],
                                 [prop for prop in original["properties"] if prop["name"] not in scheduling])

    def test_ctest_windows_reservations_run_alone_longest_first_with_smaller_parallelism(self):
        rows = self.contract_registration_inventory(True)
        # One representative per distinct scheduling cost exercises every tier.
        # COST values are deliberate policy pins checked against the source above.
        by_cost = {}
        for name, cost in self.contract_priorities().items():
            by_cost.setdefault(cost, name)
        self.assertTrue(by_cost)
        names = [by_cost[cost] for cost in sorted(by_cost, reverse=True)]
        probe = self.root / "probe.py"
        probe.write_text(
            "import pathlib, sys, time\n"
            "root = pathlib.Path(sys.argv[1])\n"
            "active = root / 'active'\n"
            "active.mkdir()\n"
            "try:\n"
            "    with (root / 'order').open('a', encoding='utf-8') as output:\n"
            "        output.write(sys.argv[2] + '\\n')\n"
            "    time.sleep(0.05)\n"
            "finally:\n"
            "    active.rmdir()\n", encoding="utf-8")
        for parallel in (4, 2):
            build = self.root / f"scheduler-{parallel}"
            build.mkdir()
            declarations = []
            # Reverse registration order to prove COST drives fresh scheduling.
            for name in reversed(names):
                row = next(row for row in rows if row["name"] == name)
                props = {prop["name"]: prop["value"] for prop in row["properties"]}
                self.assertEqual(props["PROCESSORS"], shards.PROFILES["msvc"][1])
                self.assertEqual(props["PROCESSORS"], shards.PROFILES["ucrt64"][1])
                declarations.append(f'add_test({name} "{Path(sys.executable).as_posix()}" -I -B '
                                    f'"{probe.as_posix()}" "{build.as_posix()}" {name})')
                declarations.append(f'set_tests_properties({name} PROPERTIES PROCESSORS {props["PROCESSORS"]} '
                                    f'COST {props["COST"]} TIMEOUT {props["TIMEOUT"]})')
            (build / "CTestTestfile.cmake").write_text("\n".join(declarations) + "\n", encoding="utf-8")
            result = subprocess.run(["ctest", "--test-dir", str(build), "--parallel", str(parallel),
                                     "--no-tests=error", "--output-on-failure"],
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual((build / "order").read_text(encoding="utf-8").splitlines(), names)
            self.assertFalse((build / "active").exists())

    def test_added_registration_is_reserved_and_inventory_drift_is_detected(self):
        self.root = self.root / "contract café"
        extra = ('    add_test(NAME AddedContract COMMAND "${Python3_EXECUTABLE}" -I -B '
                 '"${CMAKE_CURRENT_SOURCE_DIR}/ci/test_ci_paths.py")\n'
                 '    set_tests_properties(AddedContract PROPERTIES LABELS "ci;tools" TIMEOUT 30)\n')
        for standalone in (False, True):
            for windows in (False, True):
                rows = self.contract_registration_inventory(windows, standalone, extra=extra)
                added = next(row for row in rows if row["name"] == "AddedContract")
                props = {prop["name"]: prop["value"] for prop in added["properties"]}
                self.assertEqual(props.get("PROCESSORS", 1), 4 if windows else 1)
                existing = next(row for row in rows if row["name"] == "ExistingCMakeContract")
                self.assertNotIn("PROCESSORS", {prop["name"] for prop in existing["properties"]})
        declarations = [{"cmd": "add_test", "args": ["NAME", "AddedContract"]},
                        {"cmd": "set_tests_properties", "args": [
                            "AddedContract", "PROPERTIES", "LABELS", "ci;tools"]}]
        row = test("AddedContract", LABELS=["ci", "tools"])
        self.assert_registration_inventory([row], declarations)
        for rows in ([], [row, row], [test("AddedContract")]):
            with self.subTest(rows=rows), self.assertRaises(AssertionError):
                self.assert_registration_inventory(rows, declarations)

    def shared_gl_inventory(self):
        locked = [test("RenderTraversalIntegrated", RESOURCE_LOCK=["luminumbra_gl_context"]),
                  test("RenderTraversalEmbeddedPresetRefusal", RESOURCE_LOCK=["luminumbra_gl_context"])]
        manual = [test(name, LABELS=["manual", "perf", "gpu"], RESOURCE_LOCK=["luminumbra_gl_context"])
                  for name in ("ShieldRtBasicGpu", "ShieldRtSpikeGpu")]
        tests = [manual[0], *self.tests, locked[0], manual[1], locked[1],
                 test("SimBudgetWorldParity.First"), test("SimBudgetWorldParity.Second")]
        return tests, shards.filtered_tests(tests), {row["name"] for row in locked}

    def test_excluded_manual_gl_locks_preserve_exact_deterministic_selected_groups(self):
        tests, selected, locked = self.shared_gl_inventory()
        # A nonexecuting manual test may also bridge two otherwise independent locks.
        tests += [test("ManualBridge", LABELS=["manual-gpu"],
                       RESOURCE_LOCK=["luminumbra_gl_context", "other-context"]),
                  test("OtherRender.First", RESOURCE_LOCK=["other-context"]),
                  test("OtherRender.Second", RESOURCE_LOCK=["other-context"])]
        selected = shards.filtered_tests(tests)
        names = [row["name"] for row in selected]
        for profile in shards.PROFILES:
            for timings in (None, {names[0]: 0.1}):
                with self.subTest(profile=profile, timings=timings):
                    parts = shards.partition(tests, names, profile, timings)
                    shards.validate_union(names, parts)
                    self.assertEqual(len(sum(parts, [])), len(names))
                    self.assertEqual(set(sum(parts, [])), set(names))
                    self.assertEqual(sum(bool(locked & set(part)) for part in parts), 1)
                    self.assertEqual(sum(bool({"OtherRender.First", "OtherRender.Second"} & set(part))
                                         for part in parts), 1)
                    self.assertTrue(all(part == [name for name in names if name in part] for part in parts))
                    self.assertEqual(parts, shards.partition(tests, names, profile, timings))
                    if profile == "asan":
                        self.assertEqual(sorted(sum("SimBudgetWorldParity." in name for name in part)
                                               for part in parts), [0, 0, 2])
                    else:
                        reversed_parts = shards.partition(list(reversed(tests)), list(reversed(names)),
                                                          profile, timings)
                        self.assertEqual([set(part) for part in parts], [set(part) for part in reversed_parts])

    def test_shared_relationship_contract_reaches_every_partition_path(self):
        rows, selected, locked = self.shared_gl_inventory()
        names = [row["name"] for row in selected]
        before = copy.deepcopy(rows)
        operations = (shards.partition, shards._partition_validated, shards._partition_projected)
        for profile in shards.PROFILES:
            expected = self.relationship_oracle(rows, names, profile, None)
            for operation in operations:
                with self.subTest(profile=profile, operation=operation.__name__), patch.object(
                        shards, "relationship_groups", wraps=shards.relationship_groups) as grouping:
                    self.assertEqual(operation(rows, names, profile), expected)
                    grouping.assert_called_once_with(rows, names, profile)
                    self.assertEqual(rows, before)
                for eligible in ([], names + names[:1], names + ["unknown"]):
                    with self.subTest(profile=profile, eligible=eligible), self.assertRaisesRegex(
                            ValueError, "eligible"):
                        operation(rows, eligible, profile)
                for malformed, message in (
                        ({**test("invalid"), "command": []}, "command"),
                        (test("invalid", LABELS=[None]), "relationship"),
                        (test("invalid", LABELS=["manual"], RESOURCE_LOCK=[None]), "relationship"),
                        (test("invalid", LABELS=["manual"], DEPENDS=["missing"]), "unknown CTest dependency"),
                        (test("invalid", LABELS=["manual"], DEPENDS=[names[0]]), "closure")):
                    changed = [*rows, malformed]
                    snapshot = copy.deepcopy(changed)
                    with self.subTest(profile=profile, malformed=malformed), self.assertRaisesRegex(
                            ValueError, message):
                        operation(changed, names, profile)
                    self.assertEqual(changed, snapshot)

    def test_asan_suite_sizes_and_timing_validation_reach_every_path(self):
        for count in range(6):
            rows = [*self.tests, *(test(f"SimBudgetWorldParity.Case{index}") for index in range(count))]
            names = [row["name"] for row in rows]
            for operation in (shards.partition, shards._partition_validated, shards._partition_projected):
                with self.subTest(count=count, operation=operation.__name__):
                    parts = operation(rows, names, "asan")
                    self.assertEqual(parts, self.relationship_oracle(rows, names, "asan", None))
                    shards.validate_union(names, parts)
                    with self.assertRaisesRegex(ValueError, "invalid timings"):
                        operation(rows, names, "asan", {"ignored": True})

    def test_manual_gl_plans_keep_exact_filtered_indices_and_merge_evidence_in_all_profiles(self):
        tests, selected, locked = self.shared_gl_inventory()
        names = [row["name"] for row in selected]
        for profile in shards.PROFILES:
            for shard in (shards.shard_ids(profile) if profile == "asan" else (0, 1, 2)):
                with self.subTest(profile=profile, shard=shard):
                    directory = self.root / f"shard-{shard}"
                    manifest = self.write_plan(directory, shard, selected, tests, profile)
                    parts = manifest["partitions"]
                    shards.validate_union(names, parts)
                    self.assertEqual(sum(bool(locked & set(part)) for part in parts), 1)
                    self.assertEqual(manifest["inventory_digest"],
                                     shards.evidence_for(selected, parts, profile,
                                                         roots=manifest["roots"])["inventory_digest"])
                    start, end, stride, *extra = map(int, (directory / "ctest.shard-index.txt").read_text().split(","))
                    indices = [*range(start, end + 1, stride), *extra]
                    assigned = parts[0 if shard == 0 else shard - 1]
                    self.assertEqual([names[index - 1] for index in indices], assigned)
                    root = ET.Element("testsuite", failures="0", errors="0", skipped="0")
                    for name in assigned:
                        ET.SubElement(root, "testcase", name=name, status="run")
                    ET.ElementTree(root).write(directory / "ctest.junit.xml")
            self.merge(profile=profile)
            self.assertEqual(sorted(case.get("name") for case in ET.parse(self.root / "merged.xml").iter("testcase")),
                             sorted(names))
            if profile != "asan":
                self.merge(full=True, profile=profile)
                self.assertEqual(sorted(case.get("name") for case in ET.parse(self.root / "merged.xml").iter("testcase")),
                                 sorted(names))

    def test_manual_gl_selection_reaches_shared_and_compatibility_cli_planners(self):
        tests, selected, locked = self.shared_gl_inventory()
        tests += self.policy_roster("gcc")
        selected = shards.filtered_tests(tests)
        for filename, rows in (("ctest.all.json", tests), ("ctest.inventory.json", selected)):
            (self.root / filename).write_text(json.dumps({"tests": rows}), encoding="utf-8")
        for script, profile in (("asan_shards.py", "asan"), ("ctest_shards.py", "asan"),
                                ("ctest_shards.py", "gcc"), ("ctest_shards.py", "msvc"),
                                ("ctest_shards.py", "ucrt64")):
            with self.subTest(script=script, profile=profile):
                result = subprocess.run(
                    [sys.executable, "-I", "-B", str(Path(__file__).with_name(script)), "plan",
                     "--directory", str(self.root), "--profile", profile, "--shard", "1"],
                    capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                manifest = shards.read_json(self.root / "ctest.partition.json")
                shards.validate_union([row["name"] for row in selected], manifest["partitions"])
                self.assertEqual(sum(bool(locked & set(part)) for part in manifest["partitions"]), 1)

    def test_manual_fixture_and_dependency_edges_still_refuse_before_writing_every_plan(self):
        edges = ([test("selected", DEPENDS=["manual"]), test("manual", LABELS=["manual"])],
                 [test("selected"), test("manual", LABELS=["manual"], DEPENDS=["selected"])],
                 [test("selected", FIXTURES_REQUIRED=["scene"]),
                  test("manual", LABELS=["manual"], FIXTURES_SETUP=["scene"])],
                 [test("selected", FIXTURES_SETUP=["scene"]),
                  test("manual", LABELS=["manual"], FIXTURES_CLEANUP=["scene"])],
                 [test("selected", DEPENDS=["middle"]), test("middle", DEPENDS=["manual"]),
                  test("manual", LABELS=["manual"])])
        for profile in shards.PROFILES:
            for shard in (shards.shard_ids(profile) if profile == "asan" else (0, 1, 2)):
                for related in edges:
                    tests = [*self.tests, test("SimBudgetWorldParity.First"),
                             test("SimBudgetWorldParity.Second"), *related]
                    for filename, rows in (("ctest.all.json", tests),
                                           ("ctest.inventory.json", shards.filtered_tests(tests))):
                        (self.root / filename).write_text(json.dumps({"tests": rows}), encoding="utf-8")
                    for filename in ("ctest.partition.json", "ctest.shard-index.txt"):
                        (self.root / filename).write_bytes(b"previous evidence\n")
                    before = {path: path.read_bytes() for path in self.root.iterdir()}
                    with self.subTest(profile=profile, shard=shard, related=related), \
                            self.assertRaisesRegex(ValueError, "closure"):
                        shards.plan(self.root, shard, profile)
                    self.assertEqual({path: path.read_bytes() for path in self.root.iterdir()}, before)

    def test_nonmanual_omissions_and_selected_manual_locks_keep_complete_closure(self):
        for profile in shards.PROFILES:
            for labels in ([], ["perf", "gpu"], ["Manual"]):
                related = [test("selected", RESOURCE_LOCK=["context"]),
                           test("omitted", LABELS=labels, RESOURCE_LOCK=["context"])]
                tests = [*self.tests, test("SimBudgetWorldParity.First"),
                         test("SimBudgetWorldParity.Second"), *related]
                names = [row["name"] for row in tests if row["name"] != "omitted"]
                with self.subTest(profile=profile, labels=labels), self.assertRaisesRegex(ValueError, "closure"):
                    shards.partition(tests, names, profile)
            tests, selected, locked = self.shared_gl_inventory()
            names = [row["name"] for row in tests]
            parts = shards.partition(tests, names, profile)
            all_locked = locked | {"ShieldRtBasicGpu", "ShieldRtSpikeGpu"}
            self.assertEqual(sum(bool(all_locked & set(part)) for part in parts), 1)
            self.assertEqual(set(sum(parts, [])), set(names))
            names.remove("RenderTraversalIntegrated")
            with self.subTest(profile=profile), self.assertRaisesRegex(ValueError, "closure"):
                shards.partition(tests, names, profile)

    def test_manual_lock_exemption_keeps_indirect_nonmanual_closure(self):
        related = [test("selected", RESOURCE_LOCK=["first"]),
                   test("bridge", RESOURCE_LOCK=["first", "second"]),
                   test("last", RESOURCE_LOCK=["second"]),
                   test("manual", LABELS=["manual"], RESOURCE_LOCK=["first", "second"])]
        tests = [*self.tests, test("SimBudgetWorldParity.First"), test("SimBudgetWorldParity.Second"), *related]
        for profile in shards.PROFILES:
            for omitted in ("bridge", "last"):
                names = [row["name"] for row in shards.filtered_tests(tests) if row["name"] != omitted]
                with self.subTest(profile=profile, omitted=omitted), self.assertRaisesRegex(ValueError, "closure"):
                    shards.partition(tests, names, profile)

    def test_manual_lock_exemption_does_not_hide_invalid_or_unknown_metadata(self):
        tests, selected, locked = self.shared_gl_inventory()
        names = [row["name"] for row in selected]
        for profile in shards.PROFILES:
            for eligible in ([], names + names[:1], names + ["unknown"]):
                with self.subTest(profile=profile, eligible=eligible), self.assertRaisesRegex(ValueError, "eligible"):
                    shards.partition(tests, eligible, profile)
            for manual in (test("manual", LABELS="manual"),
                           test("manual", LABELS=["manual", None]),
                           test("manual", LABELS=["manual"], RESOURCE_LOCK="context"),
                           test("manual", LABELS=["manual"], RESOURCE_LOCK=[None]),
                           {"name": "manual", "properties": [{"name": "LABELS", "value": ["manual"]}]}):
                with self.subTest(profile=profile, manual=manual), self.assertRaises(ValueError):
                    shards.partition([*tests, manual], names, profile)
            with self.subTest(profile=profile), self.assertRaisesRegex(ValueError, "unknown CTest dependency"):
                shards.partition([*tests, test("manual", LABELS=["manual"], DEPENDS=["unknown"])], names, profile)

    def test_asan_lock_and_dependency_heavy_groups_stay_together(self):
        for properties in ({"RESOURCE_LOCK": ["context"]}, {"DEPENDS": ["SimBudgetWorldParity.Second"]}):
            tests = [*self.tests, test("SimBudgetWorldParity.First", **properties),
                     test("SimBudgetWorldParity.Second", RESOURCE_LOCK=["context"])]
            for filename in ("ctest.all.json", "ctest.inventory.json"):
                (self.root / filename).write_text(json.dumps({"tests": tests}), encoding="utf-8")
            for shard in shards.shard_ids("asan"):
                for filename in ("ctest.partition.json", "ctest.shard-index.txt"):
                    (self.root / filename).write_bytes(b"previous evidence\n")
                with self.subTest(properties=properties, shard=shard):
                    shards.plan(self.root, shard, "asan")
                    parts = shards.read_json(self.root / "ctest.partition.json")["partitions"]
                    self.assertEqual(sum({"SimBudgetWorldParity.First", "SimBudgetWorldParity.Second"} <= set(part)
                                         for part in parts), 1)

    def test_manual_gl_full_inventory_mismatch_preserves_existing_plan(self):
        tests, selected, locked = self.shared_gl_inventory()
        for profile in shards.PROFILES:
            self.write_plan(self.root, 1, selected, tests, profile)
            for changed in (list(reversed(selected)), selected[:-1], tests):
                (self.root / "ctest.inventory.json").write_text(json.dumps({"tests": changed}), encoding="utf-8")
                before = {path: path.read_bytes() for path in self.root.iterdir()}
                with self.subTest(profile=profile, changed=changed), self.assertRaisesRegex(ValueError, "filtered CTest roster"):
                    shards.plan(self.root, 1, profile)
                self.assertEqual({path: path.read_bytes() for path in self.root.iterdir()}, before)
            changed = copy.deepcopy(selected)
            changed[-3]["command"] = ["different-test-binary"]
            (self.root / "ctest.inventory.json").write_text(json.dumps({"tests": changed}), encoding="utf-8")
            before = {path: path.read_bytes() for path in self.root.iterdir()}
            with self.subTest(profile=profile), self.assertRaisesRegex(ValueError, "execution inventory differs"):
                shards.plan(self.root, 1, profile)
            self.assertEqual({path: path.read_bytes() for path in self.root.iterdir()}, before)

    def test_prefab_cases_share_one_owner_for_installed_qualification(self):
        tests = self.tests + [test("PrefabDigest.MatchesIndependentSha256Vectors"),
                              test("PrefabRuntimeTest.Loads"), test("PrefabRuntimeTest.Refuses")]
        parts = shards.partition(tests, [row["name"] for row in tests])
        self.assertEqual(sorted(sum(name.startswith("Prefab") for name in part) for part in parts), [0, 3])

    def test_malformed_duplicate_and_unbuilt_inventories_refuse(self):
        bad = (None, [], {}, {"tests": []}, {"tests": [None]}, {"tests": [test("")]},
               {"tests": self.tests + self.tests[:1]}, {"tests": [test("suite_NOT_BUILT")]},
               {"tests": [{"name": "built", "command": ["target_NOT_BUILT"]}]},
               {"tests": [{"name": "built", "command": []}]},
               {"tests": [{"name": "missing-command"}]},
               {"tests": [{"name": "built", "command": [""]}]},
               {"tests": [test("bad", DEPENDS="other")]},
               {"tests": [test("bad", RESOURCE_LOCK=[None])]},
               {"tests": [{"name": "bad", "properties": [{"name": "LABELS"}]}]})
        for document in bad:
            with self.subTest(document=document), self.assertRaises(ValueError):
                shards.inventory(document)
        with self.assertRaisesRegex(ValueError, "unknown CTest dependency"):
            shards.partition([test("a", DEPENDS=["missing"]), test("b")], ["a", "b"])

    def test_json_duplicate_keys_refuse_and_utf8_bom_is_portable(self):
        path = self.root / "inventory.json"
        path.write_text('{"tests":[],"tests":[]}', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "duplicate JSON key"):
            shards.read_json(path)
        path.write_text(json.dumps({"tests": self.tests}), encoding="utf-8-sig")
        self.assertEqual(shards.inventory(shards.read_json(path)), self.tests)

    def test_manual_selection_order_and_literal_names(self):
        literal = test(r"Suite.[literal](a+b)?^$|*\path{2} with space")
        tests = [*self.tests, literal]
        manual = test("Manual", LABELS=["tools", "manual-gpu"])
        all_tests = [manual, *tests]
        for shard in shards.shard_ids("gcc"):
            manifest = self.write_plan(self.root, shard, tests, all_tests)
            start, end, stride, *extra = map(int, (self.root / "ctest.shard-index.txt").read_text().split(","))
            indices = [*range(start, end + 1, stride), *extra]
            self.assertEqual([tests[index - 1]["name"] for index in indices], manifest["partitions"][shard - 1])
        with self.assertRaisesRegex(ValueError, "filtered CTest roster"):
            self.write_plan(self.root, 1, list(reversed(tests)), all_tests)

    def test_merge_preserves_receipts_and_full_inventory(self):
        self.reports()
        self.merge()
        cases = list(ET.parse(self.root / "merged.xml").getroot().iter("testcase"))
        self.assertEqual(sorted(case.get("name") for case in cases), sorted(self.names))
        self.assertTrue(all(case.findtext("system-out") == "receipt: " + case.get("name") for case in cases))

    def test_result_metadata_refuses_on_every_native_route_without_overwriting(self):
        output = self.root / "merged.xml"
        for profile in ("gcc", "msvc", "ucrt64"):
            for full in (False, True):
                for shard in ((0,) if full else shards.shard_ids(profile)):
                    for result in (None, "completed", "suppressed", "skipped", "failed", "unknown", ""):
                        with self.subTest(profile=profile, full=full, shard=shard, result=result):
                            self.reports(full=full, profile=profile)
                            path = self.root / f"shard-{shard}/ctest.junit.xml"
                            tree = ET.parse(path)
                            if result is not None:
                                tree.getroot()[0].set("result", result)
                            tree.write(path)
                            output.write_bytes(b"unchanged")
                            if result in (None, "completed"):
                                self.merge(full=full, profile=profile)
                                self.assertNotEqual(output.read_bytes(), b"unchanged")
                            else:
                                with self.assertRaisesRegex(ValueError, "unevaluated"):
                                    self.merge(full=full, profile=profile)
                                self.assertEqual(output.read_bytes(), b"unchanged")

    def test_report_write_failure_keeps_existing_output_and_cleans_staging(self):
        self.reports()
        output = self.root / "merged.xml"
        output.write_text("unchanged")
        before = sorted(path.name for path in self.root.iterdir())
        with patch.object(shards.ET.ElementTree, "write", side_effect=OSError("write failed")), self.assertRaises(OSError):
            self.merge()
        self.assertEqual(output.read_text(), "unchanged")
        self.assertEqual(sorted(path.name for path in self.root.iterdir()), before)

    def test_full_route_is_exact_and_preserves_binary_bound_report_bytes(self):
        self.reports(full=True)
        before = (self.root / "shard-0/ctest.junit.xml").read_bytes()
        self.merge(full=True)
        self.assertEqual((self.root / "merged.xml").read_bytes(), before)
        with self.assertRaises(OSError):
            self.merge()

    def test_unequal_inventory_properties_and_digests_refuse_without_overwriting(self):
        self.reports()
        output = self.root / "merged.xml"
        output.write_text("unchanged")
        for mutate in (lambda doc: doc.update(inventory_digest="bad"),
                       lambda doc: doc.update(partition_digest="bad"),
                       lambda doc: doc.update(shard=1),
                       lambda doc: doc["partitions"][0].append("unexpected")):
            self.reports()
            path = self.root / "shard-2/ctest.partition.json"
            doc = shards.read_json(path)
            mutate(doc)
            path.write_text(json.dumps(doc))
            with self.assertRaises(ValueError):
                self.merge()
            self.assertEqual(output.read_text(), "unchanged")
        self.reports()
        self.tests[0]["properties"] = [{"name": "RUN_SERIAL", "value": True}]
        self.write_plan(self.root / "shard-2", 2)
        with self.assertRaisesRegex(ValueError, "different CTest inventories"):
            self.merge()

    def test_same_union_with_swapped_owners_is_not_the_planned_execution(self):
        self.reports()
        first = self.root / "shard-1/ctest.junit.xml"
        second = self.root / "shard-2/ctest.junit.xml"
        left, right = first.read_bytes(), second.read_bytes()
        first.write_bytes(right)
        second.write_bytes(left)
        with self.assertRaisesRegex(ValueError, "report differs"):
            self.merge()

    def test_rehashed_alternate_partition_and_unequal_names_refuse(self):
        self.reports()
        path = self.root / "shard-1/ctest.partition.json"
        manifest = shards.read_json(path)
        parts = list(reversed(manifest["partitions"]))
        manifest.update(shards.evidence_for(self.tests, parts, "gcc", roots=manifest["roots"]))
        path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ValueError, "deterministic placement"):
            self.merge()
        self.reports()
        self.write_plan(self.root / "shard-2", 2, self.tests[:-1])
        with self.assertRaisesRegex(ValueError, "different CTest inventories"):
            self.merge()

    def test_discovery_relationship_mismatch_does_not_replace_plan(self):
        self.write_plan(self.root, 1)
        before = (self.root / "ctest.partition.json").read_bytes()
        changed = [test(self.names[0], RUN_SERIAL=True), *self.tests[1:]]
        (self.root / "ctest.all.json").write_text(json.dumps({"tests": changed}))
        with self.assertRaisesRegex(ValueError, "execution inventory differs"):
            shards.plan(self.root, 1)
        self.assertEqual((self.root / "ctest.partition.json").read_bytes(), before)

    def execution_changes(self):
        changed = copy.deepcopy(self.tests)
        changed[0]["command"][0] = "different-test-binary"
        yield "command", changed
        changed = copy.deepcopy(self.tests)
        changed[0]["command"][1] = "--gtest_filter=Missing.Test"
        yield "argument", changed
        changed = copy.deepcopy(self.tests)
        changed[0]["command"].reverse()
        yield "argument-order", changed
        for executable in ("c:/CHECKOUT/build/release/test.exe", r"C:\checkout\build\release\test.exe",
                           "/work/build/runner", r"/work/build\runner"):
            changed = copy.deepcopy(self.tests)
            changed[0]["command"][0] = executable
            yield "literal-executable-" + executable, changed
        for index in (2, 3):
            changed = copy.deepcopy(self.tests)
            changed[0]["command"][index] = changed[0]["command"][index].replace("C:/checkout", "c:/CHECKOUT")
            yield ("quoted-assignment-case" if index == 2 else "nested-assignment-case"), changed
        for key, value in (("PASS_REGULAR_EXPRESSION", ["c:/CHECKOUT/MATCH"]),
                           ("FAIL_REGULAR_EXPRESSION", ["c:/CHECKOUT/MATCH"]),
                           ("SKIP_REGULAR_EXPRESSION", ["c:/CHECKOUT/MATCH"]),
                           ("ENVIRONMENT", ["DATA=c:/CHECKOUT/MATCH", 'SCRIPT=outer="DATA=C:/checkout/MATCH"']),
                           ("ENVIRONMENT", ["DATA=C:/checkout/MATCH", 'SCRIPT=outer="DATA=c:/CHECKOUT/MATCH"']),
                           ("ENVIRONMENT_MODIFICATION", ["DATA=set:c:/CHECKOUT/MATCH"])):
            changed = copy.deepcopy(self.tests)
            for prop in changed[0]["properties"]:
                if prop["name"] == key:
                    prop["value"] = value
            yield "literal-root-case-" + key + "-" + str(value), changed
        for key, value in (("ENVIRONMENT", ["MODE=changed"]),
                           ("ENVIRONMENT_MODIFICATION", ["MODE=set:changed"]),
                           ("WORKING_DIRECTORY", "/opt/alternate-work"),
                           ("WILL_FAIL", True), ("PASS_REGULAR_EXPRESSION", ["changed"]),
                           ("FAIL_REGULAR_EXPRESSION", ["changed"]), ("SKIP_RETURN_CODE", 42),
                           ("SKIP_REGULAR_EXPRESSION", ["changed"]), ("TIMEOUT", 1),
                           ("PROCESSORS", 4), ("COST", 30), ("COST", 830), ("RESOURCE_LOCK", ["python-contracts"]),
                           ("DISABLED", True), ("FUTURE_EXECUTION_PROPERTY", {"setting": [1, "new"]})):
            changed = copy.deepcopy(self.tests)
            changed[0]["properties"] = [prop for prop in changed[0]["properties"] if prop["name"] != key]
            changed[0]["properties"].append({"name": key, "value": value})
            yield key, changed
        changed = copy.deepcopy(self.tests)
        changed[0]["future_execution_field"] = ["changed"]
        yield "future-field", changed

    def test_execution_field_drift_refuses_discovery_run_and_merge_without_mutation(self):
        # Exercise the shared contract in every profile, including the ASan route.
        roots = patch.object(shards, "inventory_roots", return_value={
            "source": "C:/checkout", "build": "C:/checkout/build/release"})
        roots.start()
        self.addCleanup(roots.stop)
        self.tests += [test(f"SimBudgetWorldParity.Replay/{index}") for index in range(2)]
        self.tests[0]["command"] = ["C:/checkout/build/release/test.exe", "--gtest_filter=Suite.Case0",
                                     '--script=print("DATA=C:/checkout/MATCH")',
                                     "--nested=outer=DATA=C:/checkout/MATCH"]
        self.tests[0]["properties"] = [
            {"name": "ENVIRONMENT", "value": ["DATA=C:/checkout/MATCH", 'SCRIPT=outer="DATA=C:/checkout/MATCH"']},
            {"name": "ENVIRONMENT_MODIFICATION", "value": ["DATA=set:C:/checkout/MATCH"]},
            {"name": "WORKING_DIRECTORY", "value": "C:/checkout/build/release"},
            *({"name": key, "value": ["C:/checkout/MATCH"]} for key in
              ("PASS_REGULAR_EXPRESSION", "FAIL_REGULAR_EXPRESSION", "SKIP_REGULAR_EXPRESSION"))]
        self.names = [row["name"] for row in self.tests]
        report_builder = patch.object(self, "reports", wraps=self.reports)
        prepare_reports = report_builder.start()
        self.addCleanup(report_builder.stop)
        for profile in shards.PROFILES:
            report_inputs = None
            self.write_plan(self.root, 1)
            shards.plan(self.root, 1, profile)
            (self.root / "ctest.junit.xml").write_text("existing execution evidence")
            for change, tests in self.execution_changes():
                with self.subTest(profile=profile, change=change):
                    (self.root / "ctest.all.json").write_text(json.dumps({"tests": tests}))
                    before = {path: path.read_bytes() for path in self.root.rglob("*") if path.is_file()}
                    with self.assertRaisesRegex(ValueError, "execution inventory differs"):
                        shards.plan(self.root, 1, profile)
                    self.assertEqual({path: path.read_bytes() for path in self.root.rglob("*") if path.is_file()}, before)
                    response = subprocess.CompletedProcess([], 0, stdout=json.dumps({"tests": tests}))
                    with patch.object(shards.subprocess, "run", return_value=response) as invoke:
                        with self.assertRaisesRegex(ValueError, "execution inventory differs"):
                            shards.run(self.root, profile)
                        invoke.assert_called_once()
                    self.assertEqual({path: path.read_bytes() for path in self.root.rglob("*") if path.is_file()}, before)
                    if report_inputs is None:
                        self.reports(profile=profile)
                        paths = [self.root / f"shard-{shard}" / name for shard in (1, 2)
                                 for name in ("ctest.all.json", "ctest.inventory.json", "ctest.partition.json",
                                              "ctest.shard-index.txt", "ctest.junit.xml")]
                        # The ten mutable case-owned fixture inputs are bounded before reading.
                        self.assertLessEqual(sum(path.stat().st_size for path in paths), 32 * 1024)
                        report_inputs = {path: path.read_bytes() for path in paths}
                    else:
                        for path, contents in report_inputs.items():
                            path.write_bytes(contents)
                    # First reject tampered raw evidence against its own manifest.
                    path = self.root / "shard-2/ctest.inventory.json"
                    path.write_text(json.dumps({"tests": tests}))
                    output = self.root / "merged.xml"
                    output.write_text("unchanged")
                    evidence = {path: path.read_bytes() for path in self.root.rglob("*") if path.is_file()}
                    with self.assertRaisesRegex(ValueError, "digest mismatch"):
                        self.merge(profile=profile)
                    self.assertEqual({path: path.read_bytes() for path in self.root.rglob("*") if path.is_file()}, evidence)
                    # Also reject two self-consistent producers with different
                    # execution inputs and otherwise identical names/partitions.
                    self.write_plan(path.parent, 2, tests, profile=profile)
                    evidence = {path: path.read_bytes() for path in self.root.rglob("*") if path.is_file()}
                    with self.assertRaisesRegex(ValueError, "different CTest inventories"):
                        self.merge(profile=profile)
                    self.assertEqual({path: path.read_bytes() for path in self.root.rglob("*") if path.is_file()}, evidence)
        self.assertEqual(prepare_reports.call_count, len(shards.PROFILES))

    def test_changed_execution_roots_refuse_without_mutating_evidence_in_every_profile(self):
        original = {"source": "C:/checkout", "build": "C:/checkout/build/release"}
        self.tests += [test(f"SimBudgetWorldParity.Replay/{index}") for index in range(2)]
        response = subprocess.CompletedProcess([], 0, stdout=json.dumps({"tests": self.tests}))
        for profile in shards.PROFILES:
            self.write_plan(self.root, 1, profile=profile, roots=original)
            (self.root / "ctest.junit.xml").write_text("existing execution evidence")
            before = {path: path.read_bytes() for path in self.root.rglob("*") if path.is_file()}
            for kind in ("source", "build"):
                for root in (original[kind].upper(), original[kind].replace("/", "\\"),
                             original[kind] + "/", "D:/other", "/d/a/checkout"):
                    with self.subTest(profile=profile, kind=kind, root=root), \
                            patch.object(shards, "inventory_roots", return_value={**original, kind: root}), \
                            patch.object(shards.subprocess, "run", return_value=response) as invoke:
                        with self.assertRaisesRegex(ValueError, "execution source or build roots differ"):
                            shards.run(self.root, profile)
                        invoke.assert_called_once()
                        self.assertEqual({path: path.read_bytes() for path in self.root.rglob("*") if path.is_file()}, before)
            with patch.object(shards, "inventory_roots", return_value=original), \
                    patch.object(shards.subprocess, "run", side_effect=[response, subprocess.CompletedProcess([], 0)]) as invoke:
                self.assertEqual(shards.run(self.root, profile), 0)
                self.assertEqual(invoke.call_count, 2)
            self.assertEqual({path: path.read_bytes() for path in self.root.rglob("*") if path.is_file()}, before)

    def test_different_recorded_roots_refuse_merge_without_mutating_evidence(self):
        original = {"source": "C:/checkout", "build": "C:/checkout/build/release"}
        for kind in ("source", "build"):
            for root in (original[kind].upper(), original[kind].replace("/", "\\"),
                         original[kind] + "/", "D:/other", "/work/other"):
                with self.subTest(kind=kind, root=root):
                    self.reports()
                    manifests = []
                    for shard in shards.shard_ids("gcc"):
                        roots = {**original, **({kind: root} if shard == 2 else {})}
                        manifests.append(self.write_plan(self.root / f"shard-{shard}", shard, roots=roots))
                    self.assertNotEqual(manifests[0]["inventory_digest"], manifests[1]["inventory_digest"])
                    output = self.root / "merged.xml"
                    output.write_text("unchanged")
                    before = {path: path.read_bytes() for path in self.root.rglob("*") if path.is_file()}
                    with self.assertRaisesRegex(ValueError, "different source or build roots"):
                        self.merge()
                    self.assertEqual({path: path.read_bytes() for path in self.root.rglob("*") if path.is_file()}, before)

    def test_identical_literal_rooted_inventories_merge_in_every_profile(self):
        self.tests += [test(f"SimBudgetWorldParity.Replay/{index}") for index in range(2)]
        self.tests[0]["command"] = ["C:/checkout/build/release/test.exe",
                                     '--script=print("DATA=C:/checkout/MATCH")']
        self.tests[0]["properties"] = [
            {"name": "PASS_REGULAR_EXPRESSION", "value": ["C:/checkout/MATCH"]},
            {"name": "ENVIRONMENT", "value": ['SCRIPT=outer="DATA=C:/checkout/MATCH"']}]
        roots = {"source": "C:/checkout", "build": "C:/checkout/build/release"}
        for profile in shards.PROFILES:
            with self.subTest(profile=profile), patch.object(shards, "inventory_roots", return_value=roots):
                self.reports(profile=profile)
                self.merge(profile=profile)
                self.assertEqual(len(list(ET.parse(self.root / "merged.xml").iter("testcase"))), len(self.tests))

    def test_non_finite_execution_properties_refuse_planning_without_writing(self):
        for value in (float("inf"), float("nan")):
            tests = [test("Suite.Case", TIMEOUT=value)]
            with self.subTest(value=value), self.assertRaisesRegex(ValueError, "non-finite"):
                self.write_plan(self.root, 0, tests)
            self.assertFalse((self.root / "ctest.partition.json").exists())
            self.assertFalse((self.root / "ctest.shard-index.txt").exists())

    def test_property_order_and_diagnostic_backtrace_are_immaterial_but_values_are_not(self):
        tests = [test("Suite.Case", ENVIRONMENT=["FIRST=1", "SECOND=2"], WILL_FAIL=False)]
        tests[0]["backtrace"] = 1
        expected = shards.digest(shards.execution_inventory(tests))
        tests[0]["backtrace"] = 9
        tests[0]["properties"].reverse()
        self.assertEqual(expected, shards.digest(shards.execution_inventory(tests)))
        tests[0]["properties"][1]["value"].reverse()
        self.assertNotEqual(expected, shards.digest(shards.execution_inventory(tests)))

    def test_invalid_or_changed_roots_and_old_evidence_version_refuse(self):
        for roots in (None, [], {}, {"unexpected": "/opt/root"}, {"build": "/work/build"},
                      {"source": "relative", "build": "/work/build"},
                      {"source": "/work", "build": 0}):
            with self.subTest(roots=roots), self.assertRaises(ValueError):
                shards.evidence_for(self.tests, [self.names], "gcc", roots=roots)
        self.write_plan(self.root, 1)
        path = self.root / "ctest.partition.json"
        manifest = shards.read_json(path)
        response = subprocess.CompletedProcess([], 0, stdout=json.dumps({"tests": self.tests}))
        for change, error in (({"roots": {**manifest["roots"], "source": "/opt/foreign"}}, "roots differ"),
                              ({"version": 1}, "digest differs"), ({"version": 2}, "digest differs"),
                              ({"version": 3}, "digest differs")):
            path.write_text(json.dumps({**manifest, **change}))
            with patch.object(shards.subprocess, "run", return_value=response) as invoke:
                with self.assertRaisesRegex(ValueError, error):
                    shards.run(self.root)
                invoke.assert_called_once()
        self.reports()
        path = self.root / "shard-2/ctest.partition.json"
        manifest = shards.read_json(path)
        for version in (1, 2, 3):
            path.write_text(json.dumps({**manifest, "version": version}))
            before = {path: path.read_bytes() for path in self.root.rglob("*") if path.is_file()}
            with self.subTest(version=version), self.assertRaisesRegex(ValueError, "digest mismatch"):
                self.merge()
            self.assertEqual({path: path.read_bytes() for path in self.root.rglob("*") if path.is_file()}, before)

    def test_report_omission_duplicate_addition_failures_and_status_refuse(self):
        for mutation in ("omit", "duplicate", "unknown", "failure", "error", "skipped", "status", "totals", "root"):
            self.reports()
            path = self.root / "shard-1/ctest.junit.xml"
            tree = ET.parse(path)
            root = tree.getroot()
            if mutation == "omit":
                root.remove(root[0])
            elif mutation == "duplicate":
                root.append(root[0])
            elif mutation == "unknown":
                root[0].set("name", "unexpected")
            elif mutation == "status":
                root[0].set("status", "notrun")
            elif mutation == "totals":
                root.set("errors", "1")
            elif mutation == "root":
                root.tag = "unexpected"
            else:
                ET.SubElement(root[0], mutation)
            tree.write(path)
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                self.merge()
        self.reports()
        (self.root / "shard-2/ctest.junit.xml").write_text("broken")
        with self.assertRaises(ET.ParseError):
            self.merge()

    def test_roster_and_relationship_drift_never_launch_execution(self):
        self.write_plan(self.root, 1)
        for tests in ([*self.tests, test("appended")], self.tests[:-1], list(reversed(self.tests)),
                      [test(self.names[0], RUN_SERIAL=True), *self.tests[1:]]):
            response = subprocess.CompletedProcess([], 0, stdout=json.dumps({"tests": tests}))
            with self.subTest(tests=tests), patch.object(shards.subprocess, "run", return_value=response) as invoke:
                with self.assertRaises(ValueError):
                    shards.run(self.root)
                invoke.assert_called_once()

    def test_every_lane_rechecks_same_context_and_preserves_limits_and_exit_status(self):
        expected = {"asan": ("debug-asan", 2, 300), "gcc": ("release", 4, 180),
                    "msvc": ("release", 4, 180), "ucrt64": ("release", 4, 360)}
        self.assertEqual(shards.PROFILES, expected)
        for profile in ("gcc", "msvc", "ucrt64"):
            for shard in (0, 1, 2):
                with self.subTest(profile=profile, shard=shard):
                    self.write_plan(self.root, shard, profile=profile)
                    self.assert_execution_context(self.root, profile, shard, expected[profile])

    def assert_execution_context(self, directory, profile, shard, limits):
        resolved = directory.resolve()
        preset, parallel, timeout = limits
        selection = [] if shard == 0 else ["-I", str(resolved / "ctest.shard-index.txt")]
        expected_tests = copy.deepcopy(self.tests)
        expected_discovery = json.dumps({"tests": expected_tests})
        encode = json.dumps
        encodings = 0

        def encode_discovery(document, *args, **kwargs):
            nonlocal encodings
            if isinstance(document, dict) and document.get("tests") is self.tests:
                encodings += 1
            return encode(document, *args, **kwargs)

        with patch.object(json, "dumps", new=encode_discovery):
            discovery = json.dumps({"tests": self.tests})
            for status in (0, 8):
                response = subprocess.CompletedProcess([], 0, stdout=discovery)
                self.assertEqual(response.stdout, expected_discovery)
                with self.subTest(status=status), patch.object(shards.subprocess, "run", side_effect=[response, subprocess.CompletedProcess([], status)]) as invoke:
                    self.assertEqual(shards.run(directory, profile), status)
                    check, execute = invoke.call_args_list
                    base = ["ctest", "--preset", preset, "--test-dir", str(resolved), "-LE", "manual"]
                    self.assertEqual(check.args[0], [*base, "--show-only=json-v1"])
                    self.assertEqual(check.kwargs, {"check": True, "capture_output": True, "text": True, "encoding": "utf-8"})
                    self.assertEqual(execute.args[0], [*base, "--no-tests=error", *selection,
                                                      "--parallel", str(parallel), "--timeout", str(timeout),
                                                      "--output-junit", "ctest.junit.xml"])
                    # Both calls inherit the wrapper's cwd and environment.
                    self.assertEqual(execute.kwargs, {})
        self.assertEqual(encodings, 1)
        self.assertEqual(self.tests, expected_tests)

    def test_windows_short_alias_uses_exact_resolved_full_and_shard_contexts(self):
        resolved = self.root.resolve()

        # Model the filesystem's short-name expansion without requiring Windows
        # symlink privilege or enabled 8.3 names. All reads use real plan files.
        class WindowsBuildAlias(type(self.root)):
            def resolve(self):
                return resolved

        alias = WindowsBuildAlias("C:/Users/RUNNER~1/AppData/Local/Temp/build")
        self.assertNotEqual(str(alias), str(resolved))
        for profile, limits in (("gcc", ("release", 4, 180)), ("msvc", ("release", 4, 180)),
                                ("ucrt64", ("release", 4, 360))):
            for shard in (0, 1, 2):
                with self.subTest(profile=profile, shard=shard):
                    self.write_plan(self.root, shard, profile=profile)
                    self.assert_execution_context(alias, profile, shard, limits)

    def test_index_tampering_and_failed_discovery_never_launch_execution(self):
        self.write_plan(self.root, 1)
        (self.root / "ctest.shard-index.txt").write_text("0")
        response = subprocess.CompletedProcess([], 0, stdout=json.dumps({"tests": self.tests}))
        for result, error in ((response, ValueError),
                              (subprocess.CalledProcessError(1, "ctest"), subprocess.CalledProcessError),
                              (subprocess.CompletedProcess([], 0, stdout="broken"), ValueError)):
            with patch.object(shards.subprocess, "run", side_effect=[result]) as invoke:
                with self.assertRaises(error):
                    shards.run(self.root)
                invoke.assert_called_once()

    def test_full_execution_rechecks_roster_and_uses_no_index_filter(self):
        self.write_plan(self.root, 0)
        response = subprocess.CompletedProcess([], 0, stdout=json.dumps({"tests": self.tests}))
        with patch.object(shards.subprocess, "run", side_effect=[response, subprocess.CompletedProcess([], 0)]) as invoke:
            self.assertEqual(shards.run(self.root), 0)
        self.assertNotIn("-I", invoke.call_args_list[1].args[0])
        self.assertIn("--show-only=json-v1", invoke.call_args_list[0].args[0])

    def test_cli_missing_suite_and_invalid_shard_refuse_without_mutation(self):
        for file in ("ctest.all.json", "ctest.inventory.json"):
            (self.root / file).write_text(json.dumps({"tests": self.tests}))
        script = str(Path(__file__).with_name("ctest_shards.py"))
        for arguments, expected in ((["--shard", "1", "--require-suite", "MissingSuite."], 1),
                                    (["--shard", "0", "--profile", "asan"], 1),
                                    (["--shard", "3"], 1),
                                    (["--shard", "4"], 2)):
            result = subprocess.run([sys.executable, "-I", "-B", script, "plan", "--directory", str(self.root),
                                     *arguments], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
            self.assertFalse((self.root / "ctest.partition.json").exists())
            self.assertFalse((self.root / "ctest.shard-index.txt").exists())


    def policy_roster(self, profile="gcc"):
        # Keep real production floors, rather than reducing policy for fixtures.
        heavy = [test(name, LABELS=["heavy"]) for name in sorted(shards.HEAVY_NAMES)]
        return [test(f"Quick.Case{index}") for index in range(shards.FLOORS[profile] - 2)] + heavy

    def policy_reports(self, selection, profile="gcc", full=False):
        tests = self.policy_roster(profile)
        for shard in ((0,) if full else shards.shard_ids(profile)):
            directory = self.root / f"shard-{shard}"
            manifest = self.write_plan(directory, shard, tests=tests, profile=profile, selection=selection)
            root = ET.Element("testsuite", failures="0", errors="0", skipped="0")
            for name in manifest["partitions"][0 if full else shard - 1]:
                ET.SubElement(root, "testcase", name=name, status="run")
            ET.ElementTree(root).write(directory / "ctest.junit.xml")
        return tests

    def test_full_light_heavy_exact_unions_and_quick_refusals_preserved(self):
        self.assertEqual(shards.FLOORS, {"gcc": 2340, "msvc": 2320, "ucrt64": 2320, "asan": 2320})
        for profile in shards.PROFILES:
            tests = self.policy_roster(profile) + [test("WorldOpenRefusal.QuickCatalogCheck")]
            full = shards.select_tests(tests, profile, "full")
            light = shards.select_tests(tests, profile, "light")
            heavy = shards.select_tests(tests, profile, "heavy")
            self.assertEqual(set(full), set(light) | set(heavy))
            self.assertFalse(set(light) & set(heavy))
            self.assertEqual(set(heavy), shards.HEAVY_NAMES)
            self.assertIn("WorldOpenRefusal.QuickCatalogCheck", light)
            for selection in (full, light, heavy):
                with self.subTest(profile=profile, selection=len(selection)):
                    if profile == "asan" and selection == heavy:
                        with self.assertRaisesRegex(ValueError, "empty CTest shard"):
                            shards._partition_validated(tests, selection, profile)
                    else:
                        parts = shards._partition_validated(tests, selection, profile)
                        shards.validate_union(selection, parts)

    def test_all_full_and_light_unions_preserve_floors_and_required_cases(self):
        for profile, filename in (("asan", "asan.yml"), ("gcc", "ci.yml"),
                                  ("msvc", "windows-build.yml"), ("ucrt64", "windows-build.yml")):
            workflow = (Path(__file__).resolve().parents[2] / ".github/workflows" / filename).read_text(encoding="utf-8")
            aggregate = {"asan": "asan", "gcc": "linux-result", "msvc": "windows-msvc-result",
                         "ucrt64": "windows-ucrt64-result"}[profile]
            section = workflow.split("\n  " + aggregate + ":\n", 1)[1]
            section = re.split(r"\n  [\w-]+:\n", section, maxsplit=1)[0]
            command = next(line for line in section.splitlines()
                           if line.strip().startswith("python3 tools/ci/check_ctest_junit.py "))
            arguments = shlex.split(command)[3:]
            required = [arguments[index + 1] for index, argument in enumerate(arguments)
                        if argument == "--require-test"]
            self.assertGreaterEqual(len(required), 3)
            tests = self.policy_roster(profile)
            tests[:len(required)] = [test(name) for name in required]
            output = self.root / "merged.xml"
            checker = str(Path(__file__).with_name("check_ctest_junit.py"))
            for selection, minimum in (("full", shards.FLOORS[profile]), ("light", shards.FLOORS[profile] - 2)):
                with self.subTest(profile=profile, selection=selection):
                    for shard in shards.shard_ids(profile):
                        directory = self.root / f"shard-{shard}"
                        manifest = self.write_plan(directory, shard, tests=tests, profile=profile, selection=selection)
                        report = ET.Element("testsuite", failures="0", errors="0", skipped="0")
                        for name in manifest["partitions"][shard - 1]:
                            ET.SubElement(report, "testcase", name=name, status="run")
                        ET.ElementTree(report).write(directory / "ctest.junit.xml")
                    shards.merge(self.root, output, profile=profile, selection=selection)
                    arguments[1] = str(minimum)
                    result = subprocess.run([sys.executable, "-I", "-B", checker, str(output), *arguments],
                                            capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual(len(list(ET.parse(output).iter("testcase"))), minimum)
                    complete = output.read_bytes()
                    for missing in required:
                        report = ET.fromstring(complete)
                        next(case for case in report.iter("testcase") if case.get("name") == missing).set(
                            "name", "Unrelated.Replacement")
                        ET.ElementTree(report).write(output)
                        result = subprocess.run([sys.executable, "-I", "-B", checker, str(output), *arguments],
                                                capture_output=True, text=True, timeout=10)
                        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                        self.assertIn(f"Required CTest cases are missing:\n  - {missing}\n", result.stderr)

    def test_floor_precedes_selection_and_refusal_does_not_replace_existing_plan(self):
        for selection in ("full", "light", "heavy"):
            for profile in shards.PROFILES:
                tests = self.policy_roster(profile)
                self.write_plan(self.root, 1, tests=tests, profile=profile, selection="full")
                before = {name: (self.root / name).read_bytes() for name in
                          ("ctest.partition.json", "ctest.shard-index.txt")}
                # Both the floor and heavy roster are wrong; floor must fire first.
                tests = tests[:-1]
                for name in ("ctest.all.json", "ctest.inventory.json"):
                    (self.root / name).write_text(json.dumps({"tests": tests}))
                with self.subTest(profile=profile, selection=selection), self.assertRaisesRegex(ValueError, "below floor"):
                    shards.plan(self.root, 1, profile, selection=selection)
                for name, content in before.items():
                    self.assertEqual((self.root / name).read_bytes(), content)

    def test_renamed_missing_unlabelled_extra_or_manual_heavy_cases_refuse(self):
        for mutation in ("rename", "missing", "unlabelled", "extra", "manual"):
            tests = self.policy_roster() + [test("Extra.Quick")]
            if mutation == "rename":
                tests[-2]["name"] += "Renamed"
            elif mutation == "missing":
                tests.pop(-2)
            elif mutation == "unlabelled":
                tests[-2]["properties"] = []
            elif mutation == "extra":
                tests[0]["properties"] = [{"name": "LABELS", "value": ["heavy"]}]
            else:
                tests[-2]["properties"][0]["value"].append("manual")
            for selection in ("full", "light", "heavy"):
                with self.subTest(mutation=mutation, selection=selection), self.assertRaisesRegex(ValueError, "heavy roster"):
                    shards.select_tests(shards.filtered_tests(tests), "gcc", selection)

    def test_reviewed_cost_hints_use_discovered_lists_and_refuse_missing_cases(self):
        root = Path(__file__).resolve().parents[2]
        cmake = (root / "test/CMakeLists.txt").read_text()
        hook = (root / "test/HeavyTestsCTest.cmake").read_text()
        self.assertIn('PROPERTIES LABELS heavy COST 830)', hook)
        expected = {
            name: ("far_lod_worker_test_TESTS" if name.startswith("FarLodWorker.")
                   else "common_tests_TESTS", str(cost))
            for name, cost in COST_HINTS.items() if name not in shards.HEAVY_NAMES
        }
        actual = {name: (test_list, cost) for test_list, name, cost in
                  re.findall(r'"(\w+_TESTS);([^";]+);(\d+)"', hook)}
        self.assertEqual(actual, expected)
        self.assertIn('list(FIND LUMINUMBRA_WORLD_OPEN_TESTS "${_lum_heavy_name}" _lum_heavy_index)', hook)
        for kind in ("heavy", "cost"):
            message = "heavy" if kind == "heavy" else "long"
            indent = "        " if kind == "heavy" else "    "
            self.assertIn(f'{indent}if(_lum_{kind}_index EQUAL -1)\n'
                          f'{indent}    message(FATAL_ERROR "Reviewed {message} correctness case missing: '
                          f'${{_lum_{kind}_name}}")\n{indent}endif()', hook)
        self.assertIn('list(FIND ${_lum_cost_test_list} "${_lum_cost_name}" _lum_cost_index)', hook)
        self.assertIn('set_tests_properties("${_lum_cost_name}" PROPERTIES COST "${_lum_cost_seconds}")', hook)
        self.assertIn('TEST_LIST ${_lum_gtest_target}_TESTS\n'
                      '            PROPERTIES RESOURCE_LOCK luminumbra_gl_context)', cmake)
        self.assertIn('TEST_LIST ${_lum_gtest_target}_TESTS)', cmake)
        self.assertIn("NO_PRETTY_VALUES\n            TEST_LIST LUMINUMBRA_WORLD_OPEN_TESTS\n            PROPERTIES TIMEOUT 1800", cmake)
        self.assertGreater(cmake.index('"${CMAKE_CURRENT_SOURCE_DIR}/HeavyTestsCTest.cmake"'),
                           cmake.rindex("TEST_LIST "))

    def test_cost_hints_preserve_selections_partitions_and_indices_in_every_profile(self):
        for profile in shards.PROFILES:
            tests = self.policy_roster(profile) + [test(name) for name in COST_HINTS if name not in shards.HEAVY_NAMES]
            hinted = copy.deepcopy(tests)
            for row in hinted:
                if row["name"] in COST_HINTS:
                    row["properties"].append({"name": "COST", "value": COST_HINTS[row["name"]]})
            for selection in ("full", "light", "heavy"):
                with self.subTest(profile=profile, selection=selection):
                    self.assertEqual(shards.select_tests(tests, profile, selection),
                                     shards.select_tests(hinted, profile, selection))
                    if profile == "asan" and selection == "heavy":
                        for rows in (tests, hinted):
                            with self.assertRaisesRegex(ValueError, "empty CTest shard"):
                                self.write_plan(self.root, 1, tests=rows, profile=profile, selection=selection)
                        continue
                    for shard in shards.shard_ids(profile):
                        before = self.write_plan(self.root, shard, tests=tests, profile=profile, selection=selection)
                        indices = (self.root / "ctest.shard-index.txt").read_bytes()
                        after = self.write_plan(self.root, shard, tests=hinted, profile=profile, selection=selection)
                        if profile == "asan":
                            self.assertNotEqual(after["partitions"], before["partitions"])
                            self.assertNotEqual(after["partition_digest"], before["partition_digest"])
                            self.assertEqual(after["partitions"], self.relationship_oracle(
                                hinted, shards.select_tests(hinted, profile, selection), profile, None))
                        else:
                            self.assertEqual((self.root / "ctest.shard-index.txt").read_bytes(), indices)
                            self.assertEqual(after["partitions"], before["partitions"])
                            self.assertEqual(after["partition_digest"], before["partition_digest"])
                        self.assertNotEqual(after["inventory_digest"], before["inventory_digest"])
                        self.assertEqual(after["timings"], before["timings"])

    def test_ctest_loads_heavy_hook_preserving_properties_and_adding_cost_hints(self):
        root = Path(__file__).resolve().parents[2]
        cmake = (root / "test/CMakeLists.txt").read_text()
        self.assertIn("NO_PRETTY_VALUES\n            TEST_LIST LUMINUMBRA_WORLD_OPEN_TESTS\n            PROPERTIES TIMEOUT 1800", cmake)
        self.assertGreater(cmake.index('"${CMAKE_CURRENT_SOURCE_DIR}/HeavyTestsCTest.cmake"'),
                           cmake.index("TEST_LIST LUMINUMBRA_WORLD_OPEN_TESTS"))
        ctest = shutil.which("ctest")
        self.assertIsNotNone(ctest, "CTest is required for heavy-label discovery regressions")
        fixture = self.root / "ctest label fixture"
        fixture.mkdir()
        heavy = sorted(shards.HEAVY_NAMES)
        quick = ["WorldOpenRefusal.QuickCatalogCheck", "WorldOpenRefusal.QuickUnlabelled"]
        long = [name for name in COST_HINTS if name not in shards.HEAVY_NAMES]
        light = [*quick, *long]
        names = [*heavy, *light]
        # Model GoogleTest's generated discovery file, including its TEST_LIST.
        discovery = "\n".join(
            f'add_test([==[{name}]==] [==[{Path(sys.executable).as_posix()}]==])\n'
            f'set_tests_properties([==[{name}]==] PROPERTIES TIMEOUT 1800)'
            for name in names
        )
        discovery += (
            f'\nset_tests_properties([==[{heavy[0]}]==] PROPERTIES LABELS "parity;simulation")\n'
            f'set_tests_properties([==[{quick[0]}]==] PROPERTIES LABELS "quick;refusal")\n'
            f'set_tests_properties([==[{long[0]}]==] PROPERTIES RESOURCE_LOCK luminumbra_gl_context)\n'
        )
        for variable, roster in (("LUMINUMBRA_WORLD_OPEN_TESTS", [*heavy, *quick]),
                                 ("far_lod_worker_test_TESTS", long[:1]),
                                 ("common_tests_TESTS", long[1:])):
            discovery += f'set({variable} ' + " ".join(f'[==[{name}]==]' for name in roster) + ')\n'
        discovered = fixture / "discovered.cmake"
        discovered.write_text(discovery, encoding="utf-8")
        testfile = fixture / "CTestTestfile.cmake"
        # TEST_INCLUDE_FILES emits ordered include() calls in CTestTestfile.cmake.
        includes = f'include([==[{discovered.as_posix()}]==])\n'
        testfile.write_text(includes, encoding="utf-8")

        def discover(*filters):
            return subprocess.run(
                [ctest, "--test-dir", str(fixture), "--show-only=json-v1", *filters],
                capture_output=True, text=True, timeout=30,
            )

        before = discover()
        self.assertEqual(before.returncode, 0, before.stdout + before.stderr)
        expected = json.loads(before.stdout)["tests"]
        hook = root / "test/HeavyTestsCTest.cmake"
        testfile.write_text(includes + f'include([==[{hook.as_posix()}]==])\n', encoding="utf-8")
        for filters, selected in (((), names), (("-L", "heavy"), heavy), (("-LE", "heavy"), light)):
            with self.subTest(filters=filters):
                result = discover(*filters)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                actual = json.loads(result.stdout)["tests"]
                self.assertEqual([row["name"] for row in actual], selected)
                for row in actual:
                    original = copy.deepcopy(next(item for item in expected if item["name"] == row["name"]))
                    if row["name"] in heavy:
                        labels = next((prop for prop in original["properties"] if prop["name"] == "LABELS"), None)
                        if labels is None:
                            original["properties"].append({"name": "LABELS", "value": ["heavy"]})
                        else:
                            labels["value"] = sorted([*labels["value"], "heavy"])
                    if row["name"] in COST_HINTS:
                        original["properties"].append({"name": "COST", "value": COST_HINTS[row["name"]]})
                    self.assertEqual(row["command"], original["command"])
                    self.assertEqual(sorted(row["properties"], key=lambda prop: prop["name"]),
                                     sorted(original["properties"], key=lambda prop: prop["name"]))
        for name in COST_HINTS:
            with self.subTest(renamed=name):
                discovered.write_text(discovery.replace(name, name + "Renamed"), encoding="utf-8")
                result = discover()
                self.assertNotEqual(result.returncode, 0)
                kind = "heavy" if name in shards.HEAVY_NAMES else "long"
                self.assertIn(f"Reviewed {kind} correctness case missing: {name}", " ".join(result.stderr.split()))

    def heavy_hook_fixture(self, names, roster):
        ctest = shutil.which("ctest")
        self.assertIsNotNone(ctest, "CTest is required for heavy-label discovery regressions")
        fixture = Path(tempfile.mkdtemp(prefix="ctest partial fixture ", dir=self.root))
        discovery = "\n".join(
            f'add_test([==[{name}]==] [==[{Path(sys.executable).as_posix()}]==] --fixture-only)\n'
            f'set_tests_properties([==[{name}]==] PROPERTIES '
            f'TIMEOUT 37 WORKING_DIRECTORY [==[{fixture.as_posix()}]==] '
            'ENVIRONMENT "DISCOVERY_FIXTURE=retained" LABELS "partial;unrelated" RUN_SERIAL TRUE)'
            for name in names
        )
        if roster is not None:
            # set(VAR) unsets VAR; an explicit empty string models a defined empty list.
            discovery += '\nset(LUMINUMBRA_WORLD_OPEN_TESTS "' + ";".join(roster) + '")\n'
        discovered = fixture / "discovered.cmake"
        discovered.write_text(discovery, encoding="utf-8")
        includes = f'include([==[{discovered.as_posix()}]==])\n'
        testfile = fixture / "CTestTestfile.cmake"
        testfile.write_text(includes, encoding="utf-8")

        def discover(*filters):
            return subprocess.run(
                [ctest, "--test-dir", str(fixture), "--show-only=json-v1", *filters],
                capture_output=True, text=True, timeout=30,
            )

        return testfile, includes, discover

    def test_ctest_partial_discovery_without_target_list_preserves_selection(self):
        hook = Path(__file__).resolve().parents[2] / "test/HeavyTestsCTest.cmake"
        unrelated = ["Preview.Frame", "Rhi.Bringup"]
        for placeholder in ([], ["world_open_refusal_test_NOT_BUILT"]):
            names = [*unrelated, *placeholder]
            testfile, includes, discover = self.heavy_hook_fixture(names, None)
            # Non-GoogleTest registrations follow the discovery includes in CTest.
            continued = (
                f'add_test(RenderTraversalIntegrated [==[{Path(sys.executable).as_posix()}]==] --fixture-only)\n'
                'set_tests_properties(RenderTraversalIntegrated PROPERTIES LABELS "partial;runner" TIMEOUT 37)\n'
            )
            names.append("RenderTraversalIntegrated")
            testfile.write_text(includes + continued, encoding="utf-8")
            selections = (
                ((), names),
                (("-R", "^Preview\\.Frame$"), unrelated[:1]),
                (("-R", "^Rhi\\.Bringup$"), unrelated[1:]),
                (("-R", "^RenderTraversalIntegrated$"), [names[-1]]),
                (("-L", "partial"), names),
                (("-LE", "heavy"), names),
                (("-L", "heavy"), []),
            )
            expected = {}
            for filters, selected in selections:
                before = discover(*filters)
                self.assertEqual(before.returncode, 0, before.stdout + before.stderr)
                expected[filters] = json.loads(before.stdout)["tests"]
                self.assertEqual([row["name"] for row in expected[filters]], selected)
            testfile.write_text(includes + f'include([==[{hook.as_posix()}]==])\n' + continued, encoding="utf-8")
            for filters, selected in selections:
                with self.subTest(placeholder=placeholder, filters=filters):
                    result = discover(*filters)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    actual = json.loads(result.stdout)["tests"]
                    self.assertEqual([row["name"] for row in actual], selected)
                    self.assertEqual(actual, expected[filters])

    def test_ctest_cost_hints_follow_each_discovered_target_in_partial_builds(self):
        hook = Path(__file__).resolve().parents[2] / "test/HeavyTestsCTest.cmake"
        groups = (
            ("LUMINUMBRA_WORLD_OPEN_TESTS", sorted(shards.HEAVY_NAMES)),
            ("far_lod_worker_test_TESTS", [name for name in COST_HINTS if name.startswith("FarLodWorker.")]),
            ("common_tests_TESTS", [name for name in COST_HINTS
                                    if name not in shards.HEAVY_NAMES and not name.startswith("FarLodWorker.")]),
        )
        # Seven nonempty subsets cover each target alone and every combination.
        for mask in range(1, 8):
            selected = [group for index, group in enumerate(groups) if mask & (1 << index)]
            names = ["Preview.Frame", *(name for _, roster in selected for name in roster)]
            testfile, includes, discover = self.heavy_hook_fixture(names, None)
            declarations = "".join(f'set({variable} "' + ";".join(roster) + '")\n'
                                   for variable, roster in selected)
            declarations += "".join(f'set_tests_properties([==[{name}]==] PROPERTIES COST 25 PROCESSORS 4)\n'
                                    for name in names)
            testfile.write_text(includes + declarations, encoding="utf-8")
            before = discover()
            self.assertEqual(before.returncode, 0, before.stdout + before.stderr)
            expected = json.loads(before.stdout)["tests"]
            testfile.write_text(includes + declarations + f'include([==[{hook.as_posix()}]==])\n',
                                encoding="utf-8")
            with self.subTest(targets=[variable for variable, _ in selected]):
                result = discover()
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                actual = json.loads(result.stdout)["tests"]
                self.assertEqual([row["name"] for row in actual], names)
                for row, original in zip(actual, expected):
                    for prop in original["properties"]:
                        if prop["name"] == "COST":
                            prop["value"] = COST_HINTS.get(row["name"], 25)
                        elif prop["name"] == "LABELS" and row["name"] in shards.HEAVY_NAMES:
                            prop["value"] = sorted([*prop["value"], "heavy"])
                    self.assertEqual(row, original)
        for variable, roster in groups[1:]:
            # Undefined targets are covered above; defined empty, false-looking,
            # partial and renamed discovery lists must still refuse.
            invalid = [[], ["OFF"], ["NOTFOUND"], ["Preview.Frame"]]
            for name in roster:
                invalid.extend(([item for item in roster if item != name],
                                [item + "Renamed" if item == name else item for item in roster]))
            for discovered in invalid:
                testfile, includes, discover = self.heavy_hook_fixture(["Preview.Frame", *roster], None)
                declarations = f'set({variable} "' + ";".join(discovered) + '")\n'
                testfile.write_text(includes + declarations + f'include([==[{hook.as_posix()}]==])\n',
                                    encoding="utf-8")
                missing = next(name for name in roster if name not in discovered)
                with self.subTest(variable=variable, roster=discovered):
                    result = discover("-R", "^Preview\\.Frame$")
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(f"Reviewed long correctness case missing: {missing}",
                                  " ".join(result.stderr.split()))

    def test_ctest_defined_invalid_target_lists_refuse_even_filtered(self):
        hook = Path(__file__).resolve().parents[2] / "test/HeavyTestsCTest.cmake"
        heavy = sorted(shards.HEAVY_NAMES)
        names = [*heavy, "Preview.Frame"]
        rosters = [[], ["OFF"], ["NOTFOUND"], ["Preview.Frame"]]
        for name in heavy:
            rosters.append([item for item in names if item != name])
            rosters.append([item + "Renamed" if item == name else item for item in names])
        for roster in rosters:
            testfile, includes, discover = self.heavy_hook_fixture(names, roster)
            before = discover()
            self.assertEqual(before.returncode, 0, before.stdout + before.stderr)
            self.assertEqual([row["name"] for row in json.loads(before.stdout)["tests"]], names)
            testfile.write_text(includes + f'include([==[{hook.as_posix()}]==])\n', encoding="utf-8")
            missing = next(name for name in heavy if name not in roster)
            for filters in ((), ("-R", "^Preview\\.Frame$")):
                with self.subTest(roster=roster, filters=filters):
                    result = discover(*filters)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(f"Reviewed heavy correctness case missing: {missing}",
                                  " ".join(result.stderr.split()))

    def test_only_explicit_false_selects_light_missing_old_or_invalid_outputs_select_full(self):
        for value in ("", "true", "False", "0", "unknown", None):
            self.assertEqual(shards.selection_policy(value), "full")
        self.assertEqual(shards.selection_policy("false"), "light")
        self.assertEqual(shards.selection_policy("", "heavy"), "heavy")
        roots = shards.inventory_roots(self.root)
        full = shards.evidence_for(self.tests, [self.names], "gcc", roots=roots, selection="full")
        light = shards.evidence_for(self.tests, [self.names], "gcc", roots=roots, selection="light")
        self.assertNotEqual(full["partition_digest"], light["partition_digest"])

    def test_cli_policy_defaults_and_explicit_selection_use_full_inventory_floor(self):
        script = str(Path(__file__).with_name("ctest_shards.py"))
        tests = self.policy_roster()
        self.write_plan(self.root, 1, tests=tests)
        for arguments, selection in (([], "full"), (["--needs-heavy", ""], "full"),
                                     (["--needs-heavy", "true"], "full"),
                                     (["--needs-heavy", "0"], "full"),
                                     (["--needs-heavy", "false"], "light"),
                                     (["--selection", "heavy"], "heavy")):
            result = subprocess.run([sys.executable, "-I", "-B", script, "plan", "--directory", str(self.root),
                                     "--shard", "1", *arguments], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            manifest = shards.read_json(self.root / "ctest.partition.json")
            self.assertEqual(manifest["selection"], selection)
            shards.validate_union(shards.select_tests(tests, "gcc", selection), manifest["partitions"])
        for name in ("ctest.all.json", "ctest.inventory.json"):
            (self.root / name).write_text(json.dumps({"tests": tests[1:]}))
        before = (self.root / "ctest.partition.json").read_bytes()
        result = subprocess.run([sys.executable, "-I", "-B", script, "plan", "--directory", str(self.root),
                                 "--shard", "1", "--needs-heavy", "false"], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 1)
        self.assertIn("full discovered inventory below floor", result.stderr)
        self.assertEqual((self.root / "ctest.partition.json").read_bytes(), before)

    def test_selected_junit_exact_coverage_and_selection_bound_digests_in_all_profiles(self):
        for profile in shards.PROFILES:
            previous = None
            for selection in ("full", "light", "heavy"):
                if profile == "asan" and selection == "heavy":
                    with self.assertRaisesRegex(ValueError, "empty CTest shard"):
                        self.policy_reports(selection, profile)
                    continue
                tests = self.policy_reports(selection, profile)
                output = self.root / "merged.xml"
                summary = self.root / "summary"
                summary.write_text("")
                with patch.dict(os.environ, GITHUB_STEP_SUMMARY=str(summary)):
                    shards.merge(self.root, output, profile, selection=selection)
                expected = shards.select_tests(tests, profile, selection)
                self.assertEqual({case.get("name") for case in ET.parse(output).iter("testcase")}, set(expected))
                receipt = shards.read_json(output.with_suffix(".selection.json"))
                self.assertEqual(receipt["selection"], selection)
                self.assertEqual(receipt["executed_count"], len(expected))
                self.assertEqual(receipt["executed_digest"], shards.digest(sorted(expected)))
                self.assertNotEqual(receipt["partition_digest"], previous)
                previous = receipt["partition_digest"]
                self.assertEqual("heavy subset deferred:" in summary.read_text(), selection == "light")

    def test_light_coverage_refuses_missing_extra_skipped_or_mismatched_selection_without_mutation(self):
        output = self.root / "merged.xml"
        receipt = output.with_suffix(".selection.json")
        self.policy_reports("light")
        inputs = [self.root / shard / name
                  for shard in ("shard-1", "shard-2")
                  for name in ("ctest.all.json", "ctest.inventory.json", "ctest.partition.json",
                               "ctest.shard-index.txt", "ctest.junit.xml")]
        # Bound the ten case-owned payloads before reading; allow platform path
        # encodings and CRLF without sharing bytes across cases or profiles.
        self.assertLessEqual(sum(path.stat().st_size for path in inputs), 2 * 1024 * 1024)
        saved = [(path, path.read_bytes()) for path in inputs]
        for mutation in ("missing", "extra", "skipped", "selection", "digest", "mixed", "floor"):
            for path, payload in saved:
                path.write_bytes(payload)
            path = self.root / "shard-1/ctest.junit.xml"
            tree = ET.parse(path)
            if mutation == "missing":
                tree.getroot().remove(tree.getroot()[0])
            elif mutation == "extra":
                ET.SubElement(tree.getroot(), "testcase", name=sorted(shards.HEAVY_NAMES)[0])
            elif mutation == "skipped":
                ET.SubElement(tree.getroot()[0], "skipped")
            elif mutation in ("selection", "digest", "mixed"):
                target = self.root / ("shard-2" if mutation == "mixed" else "shard-1") / "ctest.partition.json"
                document = shards.read_json(target)
                document["selection" if mutation != "digest" else "partition_digest"] = "full"
                target.write_text(json.dumps(document))
            else:
                target = self.root / "shard-1/ctest.inventory.json"
                document = shards.read_json(target)
                document["tests"].pop(0)
                target.write_text(json.dumps(document))
            tree.write(path)
            output.write_text("original report")
            receipt.write_text("original receipt")
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                shards.merge(self.root, output, selection="light")
            self.assertEqual(output.read_text(), "original report")
            self.assertEqual(receipt.read_text(), "original receipt")

    def test_plan_validates_two_owned_graphs_and_keeps_fresh_refusals(self):
        def graph_ids(value):
            if isinstance(value, dict):
                return (id(value), tuple((key, graph_ids(item)) for key, item in value.items()))
            if isinstance(value, list):
                return (id(value), tuple(graph_ids(item) for item in value))
            return None

        for profile, selection in (("gcc", "light"), ("asan", None)):
            rows = self.policy_roster() if profile == "gcc" else self.tests
            all_rows = copy.deepcopy(rows) + [test("Manual.Only", LABELS=["manual"])]
            files = ((self.root / "ctest.all.json", all_rows),
                     (self.root / "ctest.inventory.json", rows))
            for path, value in files:
                path.write_text(json.dumps({"tests": value}), encoding="utf-8")
            roots = shards.inventory_roots(self.root)
            selected = shards.select_tests(rows, profile, selection)
            parts = shards._partition_validated(all_rows, selected, profile)
            golden = {**shards.evidence_for(rows, parts, profile, roots=roots, selection=selection), "shard": 1}
            positions = {row["name"]: index for index, row in enumerate(rows, 1)}
            indices = [positions[name] for name in parts[0]]
            expected_index = (",".join(map(str, [indices[0], indices[0], 1, *indices[1:]])) + os.linesep).encode()
            calls = []
            original = shards.inventory

            def validate(document):
                value = original(document)
                calls.append((value, copy.deepcopy(value), graph_ids(value)))
                return value

            with patch.object(shards, "inventory", side_effect=validate), patch.object(
                    shards, "_partition_validated", wraps=shards._partition_validated) as core:
                prefab = shards.plan(self.root, 1, profile, selection=selection)
            # Independent discoveries plus shared relationship validation.
            self.assertEqual(len(calls), 3)
            self.assertIsNot(calls[0][0], calls[1][0])
            self.assertIs(calls[2][0], calls[0][0])
            self.assertEqual(core.call_count, 1)
            self.assertIs(core.call_args.args[0], calls[0][0])
            for value, content, identities in calls:
                self.assertEqual(value, content)
                self.assertEqual(graph_ids(value), identities)
            self.assertEqual((self.root / "ctest.partition.json").read_bytes(),
                             (json.dumps(golden, sort_keys=True) + os.linesep).encode("utf-8"))
            self.assertEqual((self.root / "ctest.shard-index.txt").read_bytes(), expected_index)
            self.assertEqual(prefab, any(name.startswith("PrefabRuntimeTest.") for name in parts[0]))
            if profile == "gcc":
                calls.clear()
                with patch.object(shards, "inventory", side_effect=validate), patch.object(
                        shards, "_partition_validated", wraps=shards._partition_validated) as core:
                    shards.plan(self.root, 0, profile, selection="full")
                self.assertEqual(len(calls), 3)
                self.assertIsNot(calls[0][0], calls[1][0])
                self.assertIs(calls[2][0], calls[0][0])
                core.assert_not_called()
            outputs = {name: (self.root / name).read_bytes()
                       for name in ("ctest.partition.json", "ctest.shard-index.txt")}
            for mutation in ("provenance", "command", "properties", "roots"):
                changed = copy.deepcopy(all_rows)
                if mutation == "provenance":
                    changed[0]["provenance"] = "different"
                elif mutation == "command":
                    changed[0]["command"] = ["different-command"]
                elif mutation == "properties":
                    changed[0]["properties"] = [{"name": "ENVIRONMENT", "value": ["DIFFERENT=1"]}]
                (self.root / "ctest.all.json").write_text(json.dumps({"tests": changed}), encoding="utf-8")
                current_roots = {**roots, "source": "relative"} if mutation == "roots" else roots
                with self.subTest(profile=profile, mutation=mutation), patch.object(
                        shards, "inventory_roots", return_value=current_roots), self.assertRaises(ValueError):
                    shards.plan(self.root, 1, profile, selection=selection)
                for name, payload in outputs.items():
                    self.assertEqual((self.root / name).read_bytes(), payload)
            caller_rows = copy.deepcopy(all_rows)
            shards._partition_validated(caller_rows, selected, profile)
            caller_rows[0]["command"] = []
            with self.assertRaisesRegex(ValueError, "malformed or unbuilt CTest command"):
                shards.partition(caller_rows, selected, profile)

    def test_light_refusal_fixture_is_constructed_once_with_real_boundaries(self):
        original = shards.merge
        refusals = []
        script = str(Path(__file__).with_name("ctest_shards.py"))

        def merge(*args, **kwargs):
            result = subprocess.run(
                [sys.executable, "-I", "-B", script, "merge", "--directory", str(self.root),
                 "--output", str(args[1]), "--selection", "light"],
                capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("CTest merge refused:", result.stderr)
            try:
                return original(*args, **kwargs)
            except ValueError:
                refusals.append(True)
                raise

        with patch.object(self, "policy_reports", wraps=self.policy_reports) as construct, patch.object(
                shards, "plan", wraps=shards.plan) as plans, patch.object(shards, "merge", side_effect=merge):
            self.test_light_coverage_refuses_missing_extra_skipped_or_mismatched_selection_without_mutation()
        self.assertEqual(construct.call_count, 1)
        self.assertEqual(plans.call_count, 2)
        self.assertEqual(len(refusals), 7)

    def test_case_clock_and_phase_observations_are_bounded_and_synchronous(self):
        import io
        import time

        class Stream:
            def __init__(self):
                self.operations = []

            def write(self, value):
                self.operations.append(("write", value))
                return len(value)

            def flush(self):
                self.operations.append(("flush",))

        stream = Stream()
        result = CaseProgressResult(io.StringIO(), False, 0)
        with patch.object(time, "monotonic_ns", return_value=7, create=True), \
                patch.object(sys, "stderr", stream):
            result.startTest(self)
            result.stopTest(self)
        self.assertEqual(stream.operations, [
            ("write", f"\nCASE START {self.id()}\nCLOCK START 0 00000000000000000007"),
            ("write", "\n"), ("flush",),
            ("write", f"\nCASE FINISH {self.id()}\nCLOCK FINISH 0 00000000000000000007"),
            ("write", "\n"), ("flush",)])
        self.assertEqual(result.testsRun, 1)
        for value, expected in ((0, "0 00000000000000000000"),
                                (10**20 - 1, "0 99999999999999999999"),
                                (-1, "4 00000000000000000000"),
                                (10**20, "4 00000000000000000000"),
                                (True, "4 00000000000000000000"),
                                (None, "4 00000000000000000000"),
                                (1.5, "4 00000000000000000000"),
                                ("7", "4 00000000000000000000")):
            with self.subTest(value=value), patch.object(time, "monotonic_ns", return_value=value):
                self.assertEqual(CaseProgressResult._clock(), expected)
        for error, status in ((OSError("timer unavailable"), 2),
                              (NotImplementedError("timer unsupported"), 3)):
            with self.subTest(error=type(error)), patch.object(time, "monotonic_ns", side_effect=error):
                self.assertEqual(CaseProgressResult._clock(), f"{status} 00000000000000000000")
        with patch.object(time, "monotonic_ns", None):
            self.assertEqual(CaseProgressResult._clock(), "1 00000000000000000000")
        with patch.object(time, "monotonic_ns"):
            del time.monotonic_ns
            self.assertEqual(CaseProgressResult._clock(), "1 00000000000000000000")
        for error in (PermissionError("denied"), RuntimeError("unexpected"),
                      ValueError("unexpected"), TypeError("unexpected"), KeyboardInterrupt(), SystemExit(9)):
            with self.subTest(error=type(error)), patch.object(time, "monotonic_ns", side_effect=error):
                with self.assertRaises(type(error)):
                    CaseProgressResult._clock()
        # Timer unavailability must not turn a real assertion failure into success.
        class OwnedFailure(unittest.TestCase):
            def runTest(self):
                self.fail("retained failure")

        for timer_options in ({"new": None}, {"side_effect": OSError("timer unavailable")},
                              {"side_effect": NotImplementedError("timer unsupported")},
                              {"return_value": -1}):
            result = CaseProgressResult(io.StringIO(), False, 0)
            with patch.object(time, "monotonic_ns", **timer_options), \
                    patch.object(sys, "stderr", io.StringIO()):
                OwnedFailure().run(result)
            self.assertEqual((result.testsRun, len(result.failures), len(result.errors)), (1, 1, 0))
        for operation in ("write", "flush"):
            stream = Stream()
            with patch.object(stream, operation, side_effect=OSError("stream failure")), \
                    patch.object(sys, "stderr", stream):
                with self.assertRaises(OSError):
                    CaseProgressResult(io.StringIO(), False, 0).startTest(self)
                with self.assertRaises(OSError):
                    CaseProgressResult(io.StringIO(), False, 0).stopTest(self)
                with self.assertRaises(OSError):
                    CaseProgressResult._phase("asan", "full", 1, "plan-start")
        profiles = ("asan", "gcc", "msvc", "ucrt64")
        self.assertEqual(tuple(shards.PROFILES), profiles)
        phases = ("lane-fixture-start", "lane-fixture-finish", "plan-start", "plan-finish",
                  "positive-start", "positive-finish", "run-fixture-start", "run-fixture-finish",
                  "resolved-start", "resolved-finish", "refusal-fixture-start", "refusal-fixture-finish",
                  "negative-start", "negative-finish")
        # Enumerate only scalar IDs, never execute or copy a policy input graph.
        stream = Stream()
        with patch.object(time, "monotonic_ns", return_value=10**20 - 1), \
                patch.object(sys, "stderr", stream):
            for profile in profiles:
                for phase in ("fixture-start", "fixture-finish"):
                    CaseProgressResult._phase(profile, "all", 0, phase)
                for selection in ("full", "light", "heavy"):
                    for shard in (1, 2):
                        for phase in phases:
                            CaseProgressResult._phase(profile, selection, shard, phase)
        self.assertEqual(len(stream.operations), 344 * 3)
        lines = []
        for index in range(0, len(stream.operations), 3):
            write, newline, flush = stream.operations[index:index + 3]
            self.assertEqual((write[0], newline, flush), ("write", ("write", "\n"), ("flush",)))
            line = write[1] + "\n"
            self.assertEqual(len(line.split()), 7)
            self.assertEqual(line.split()[-2:], ["0", "99999999999999999999"])
            lines.append(line)
        self.assertEqual((len(lines), sum(map(len, lines)), max(map(len, lines))), (344, 20210, 67))
        self.assertEqual(sum(len(line.replace("\n", "\r\n")) for line in lines), 20554)

    def test_case_progress_records_survive_success_failure_and_interruption(self):
        import time

        child = """import runpy, sys, threading, unittest
script, outcome = sys.argv[1:]
class OwnedCases(unittest.TestCase):
    def test_first(self):
        self.assertTrue(True)
    def test_second(self):
        if outcome == 'failure':
            self.fail('owned failure')
        if outcome == 'interruption':
            print('waiting', flush=True)
            threading.Event().wait()
sys.argv = [script, 'OwnedCases']
runpy.run_path(script, run_name='__main__', init_globals={'OwnedCases': OwnedCases})
"""
        first = "__main__.OwnedCases.test_first"
        second = "__main__.OwnedCases.test_second"
        for suite in ("test_ctest_shards.py", "test_offline_deps.py"):
            for outcome in ("success", "failure", "interruption"):
                argv = [sys.executable, "-I", "-B", "-c", child,
                        str(Path(__file__).with_name(suite)), outcome]
                with self.subTest(suite=suite, outcome=outcome):
                    if outcome == "interruption":
                        log = self.root / f"{suite}.progress.stderr"
                        ready = self.root / f"{suite}.progress.stdout"
                        with log.open("wb") as stream, ready.open("wb") as signal:
                            process = subprocess.Popen(argv, stdout=signal, stderr=stream)
                            try:
                                deadline = time.monotonic() + 10
                                while not ready.stat().st_size:
                                    if process.poll() is not None or time.monotonic() >= deadline:
                                        self.fail("owned child did not reach its waiting case")
                                    self.assertLessEqual(log.stat().st_size, 8192)
                                    time.sleep(0.005)
                                self.assertEqual(ready.read_bytes().replace(b"\r\n", b"\n"), b"waiting\n")
                                self.assertIn(b"CASE START " + second.encode() + b"\n",
                                              log.read_bytes().replace(b"\r\n", b"\n"))
                                with self.assertRaises(subprocess.TimeoutExpired):
                                    process.communicate(timeout=0)
                            finally:
                                process.kill()
                                process.communicate()
                        data = log.read_bytes()
                    else:
                        result = subprocess.run(argv, capture_output=True, timeout=10)
                        self.assertEqual(result.returncode, 1 if outcome == "failure" else 0)
                        self.assertEqual(result.stdout, b"")
                        data = result.stderr
                    self.assertLessEqual(len(data), 8192)
                    records = [line for line in data.decode().replace("\r\n", "\n").splitlines(keepends=True)
                               if line.startswith("CASE ")]
                    expected = [f"CASE START {first}\n", f"CASE FINISH {first}\n", f"CASE START {second}\n"]
                    if outcome != "interruption":
                        expected.append(f"CASE FINISH {second}\n")
                    self.assertEqual(records, expected)
                    self.assertIn(b"FAILED (failures=1)" if outcome == "failure" else
                                  b"OK" if outcome == "success" else f"CASE START {second}".encode(), data)

    def test_merge_publication_failure_restores_report_and_receipt(self):
        self.policy_reports("light")
        output = self.root / "merged.xml"
        receipt = output.with_suffix(".selection.json")
        replace = Path.replace

        def refuse_report(source, target):
            if target == output:
                raise OSError("report publication failed")
            return replace(source, target)

        for existing in (False, True):
            output.write_text("original report")
            if existing:
                receipt.write_text("original receipt")
            with patch.object(Path, "replace", refuse_report), self.assertRaises(OSError):
                shards.merge(self.root, output, selection="light")
            self.assertEqual(output.read_text(), "original report")
            self.assertEqual(receipt.exists(), existing)
            if existing:
                self.assertEqual(receipt.read_text(), "original receipt")

    def test_light_execution_uses_full_roster_indices_and_refuses_policy_drift(self):
        tests = self.policy_reports("light")
        directory = self.root / "shard-1"
        manifest = shards.read_json(directory / "ctest.partition.json")
        response = subprocess.CompletedProcess([], 0, stdout=json.dumps({"tests": tests}))
        with patch.object(shards, "inventory_roots", return_value=manifest["roots"]), patch.object(
                shards.subprocess, "run", side_effect=[response, subprocess.CompletedProcess([], 0)]) as invoke:
            self.assertEqual(shards.run(directory), 0)
            self.assertIn("-I", invoke.call_args.args[0])
        manifest["selection"] = "full"
        (directory / "ctest.partition.json").write_text(json.dumps(manifest))
        with patch.object(shards.subprocess, "run", return_value=response) as invoke, self.assertRaises(ValueError):
            shards.run(directory)
        invoke.assert_called_once()

    def test_qualification_refuses_light_and_preserves_full_report_bytes(self):
        self.policy_reports("full", full=True)
        before = (self.root / "shard-0/ctest.junit.xml").read_bytes()
        shards.merge(self.root, self.root / "merged.xml", full=True, selection="full")
        self.assertEqual((self.root / "merged.xml").read_bytes(), before)
        with self.assertRaisesRegex(ValueError, "qualification requires full"):
            shards.merge(self.root, self.root / "merged.xml", full=True, selection="light")
        with self.assertRaisesRegex(ValueError, "qualification requires full"):
            shards.plan(self.root / "shard-0", 0, selection="light")

    def test_partial_and_aggregate_only_reruns_reuse_latest_each_producer(self):
        for attempt, shard in ((1, 1), (1, 2), (2, 2)):
            path = self.root / f"gcc-junit-sha-run-shard-{shard}-attempt-{attempt}"
            path.mkdir()
            (path / "receipt").write_text(str(attempt))
        shards.collect(self.root, "gcc-junit-sha-run", 3)
        self.assertEqual((self.root / "shard-1/receipt").read_text(), "1")
        self.assertEqual((self.root / "shard-2/receipt").read_text(), "2")

    def test_collect_refuses_missing_wrong_route_foreign_and_future_artifacts(self):
        for suffix in ("shard-1-attempt-1", "shard-0-attempt-1", "shard-2-attempt-4", "foreign"):
            with tempfile.TemporaryDirectory(dir=self.root) as directory:
                root = Path(directory)
                (root / ("prefix-" + suffix)).mkdir()
                with self.subTest(suffix=suffix), self.assertRaises(ValueError):
                    shards.collect(root, "prefix", 3)
                self.assertFalse((root / "shard-1").exists())

    def test_collect_copy_failure_cleans_staged_evidence(self):
        for shard in shards.shard_ids("gcc"):
            (self.root / f"prefix-shard-{shard}-attempt-1").mkdir()
        before = sorted(path.name for path in self.root.iterdir())
        with patch.object(shards.shutil, "copytree", side_effect=OSError("copy failed")), self.assertRaises(OSError):
            shards.collect(self.root, "prefix", 1)
        self.assertEqual(sorted(path.name for path in self.root.iterdir()), before)

    def test_collect_move_failure_rolls_back_exposed_evidence(self):
        for shard in shards.shard_ids("gcc"):
            (self.root / f"prefix-shard-{shard}-attempt-1").mkdir()
        before = sorted(path.name for path in self.root.iterdir())
        rename = Path.rename

        def move(source, destination):
            if destination.name == "shard-2":
                raise OSError("move failed")
            return rename(source, destination)

        with patch.object(Path, "rename", move), self.assertRaises(OSError):
            shards.collect(self.root, "prefix", 1)
        self.assertEqual(sorted(path.name for path in self.root.iterdir()), before)

    def test_gate_refuses_failed_or_unknown_classification_on_every_route(self):
        for classification in ("failure", "cancelled", "skipped", "", "unknown"):
            for result, cpp, enabled, qualification in (
                    ("success", "true", "true", "false"),
                    ("success", "false", "true", "false"),
                    ("skipped", "false", "false", "false"),
                    ("success", "true", "true", "true")):
                with self.subTest(classification=classification, result=result,
                                  cpp=cpp, enabled=enabled, qualification=qualification):
                    with self.assertRaisesRegex(ValueError, "classification"):
                        shards.gate(result, cpp, "false", "workflow_dispatch", "",
                                    qualification, enabled, classification)

    def test_gate_all_results_cancellation_drafts_and_explicit_routes(self):
        for result in ("success", "failure", "cancelled", "skipped", "", "unknown"):
            for cancelled in ("false", "true", ""):
                for draft in ("false", "true", ""):
                    for cpp in ("false", "true", ""):
                        accepted = result == "success" and cancelled == draft == "false"
                        with self.subTest(result=result, cancelled=cancelled, draft=draft, cpp=cpp):
                            if accepted:
                                self.assertEqual(shards.gate(result, cpp, cancelled, "pull_request", draft, "false", "true", "success"),
                                                 "python-only" if cpp == "false" else "sharded")
                            else:
                                with self.assertRaises(ValueError):
                                    shards.gate(result, cpp, cancelled, "pull_request", draft, "false", "true", "success")
        self.assertEqual(shards.gate("success", "true", "false", "workflow_dispatch", "", "true", "true", "success"), "full")
        self.assertEqual(shards.gate("skipped", "true", "false", "push", "", "false", "false", "success"), "skipped")
        with self.assertRaises(ValueError):
            shards.gate("success", "false", "false", "workflow_dispatch", "", "true", "true", "success")
        with self.assertRaises(ValueError):
            shards.gate("skipped", "false", "true", "push", "", "false", "false", "success")


    @staticmethod
    def relationship_oracle(rows, selected, profile, timings):
        # Independent adjacency/traversal construction, with no union/remapping.
        import hashlib
        edges = {row["name"]: set() for row in rows}
        buckets = {}
        excluded_manual = {row["name"] for row in rows if any(
            prop["name"] == "LABELS" and any("manual" in label for label in prop["value"])
            for prop in row.get("properties", []))} - set(selected)
        for row in rows:
            name = row["name"]
            for prop in row.get("properties", []):
                kind = prop["name"]
                if kind not in ("FIXTURES_SETUP", "FIXTURES_REQUIRED", "FIXTURES_CLEANUP",
                                "DEPENDS", "RESOURCE_LOCK"):
                    continue
                if kind == "RESOURCE_LOCK" and name in excluded_manual:
                    continue
                for value in prop["value"]:
                    if kind == "DEPENDS":
                        edges[name].add(value)
                        edges[value].add(name)
                    else:
                        buckets.setdefault((kind.startswith("FIXTURES_"), value), []).append(name)
        if profile == "gcc":
            buckets["prefab"] = [name for name in selected
                                 if name.startswith(("PrefabDigest.", "PrefabRuntimeTest."))]
        if profile == "asan":
            for suite in ("SimBudgetWorldParity.", "AmbientDeterminismReceipt."):
                buckets[suite] = [name for name in selected if suite in name]
        for members in buckets.values():
            for left, right in zip(members, members[1:]):
                edges[left].add(right)
                edges[right].add(left)
        components, visited = [], set()
        selected_set = set(selected)
        for name in edges:
            if name in visited:
                continue
            component, pending = set(), [name]
            while pending:
                current = pending.pop()
                if current in component:
                    continue
                component.add(current)
                pending.extend(edges[current] - component)
            visited.update(component)
            if component.intersection(selected_set):
                if not component <= selected_set:
                    raise ValueError("closure")
                components.append(tuple(sorted(component)))
        owners = {}
        if profile == "asan":
            weights = {row["name"]: (timings or {}).get(row["name"], next(
                (prop["value"] for prop in row.get("properties", []) if prop["name"] == "COST"), 1) or 1)
                       for row in rows}
            ordered = sorted(components, key=lambda group: (-sum(weights[name] for name in group), group))
            loads = [0.0] * 3
            for component in ordered:
                owner = min(range(3), key=lambda index: (loads[index], index))
                owners.update((name, owner) for name in component)
                loads[owner] += sum(weights[name] for name in component)
        else:
            unknown = max([60.0, *(timings or {}).values()])
            weights = {component: sum((timings or {}).get(name, unknown) for name in component)
                       for component in components}
            ordered = sorted(components, key=lambda item: (-weights[item], item)) if timings else sorted(components)
            loads = [0.0, 0.0]
            for component in ordered:
                encoded = json.dumps(component, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()
                owner = min(range(2), key=lambda index: (loads[index], index)) if timings else int(
                    hashlib.sha256(encoded).hexdigest(), 16) % 2
                owners.update((name, owner) for name in component)
                loads[owner] += weights[component]
        return [[name for name in selected if owners[name] == owner] for owner in range(shards.SHARD_COUNTS[profile])]

    def test_projected_plans_match_traversal_oracle_and_literal_manifests(self):
        for profile in shards.PROFILES:
            rows = self.policy_roster(profile) + [
                test("PrefabDigest.Content", FIXTURES_SETUP=["runtime"]),
                test("PrefabRuntimeTest.Load", FIXTURES_REQUIRED=["runtime"], DEPENDS=["linked"]),
                test("cleanup", FIXTURES_CLEANUP=["runtime"]),
                test("linked", DEPENDS=["linked"], RESOURCE_LOCK=["shared", "runtime"]),
                test("writer", RESOURCE_LOCK=["shared"])]
            rows[0]["properties"] = [{"name": "DEPENDS", "value": [rows[1]["name"], rows[1]["name"]]}]
            rows[1]["properties"] = [{"name": "DEPENDS", "value": [rows[0]["name"]]}]
            rows[2]["unknown"] = {"opaque": [None, True, 17, 1.5, "résumé/世界"]}
            all_rows = copy.deepcopy(rows) + [test("Manual.Only", LABELS=["manual"])]
            all_rows[0]["backtrace"] = 91
            for filename, values in (("ctest.all.json", all_rows), ("ctest.inventory.json", rows)):
                (self.root / filename).write_text(json.dumps({"tests": values}), encoding="utf-8")
            inputs = {name: (self.root / name).read_bytes() for name in ("ctest.all.json", "ctest.inventory.json")}
            before = copy.deepcopy((rows, all_rows))
            roots = {"build": "C:/build/世界", "source": "/opt/source"}
            for selection in ("full", "light", "heavy"):
                selected = shards.select_tests(rows, profile, selection)
                if profile == "asan" and selection == "heavy":
                    with self.assertRaisesRegex(ValueError, "empty CTest shard"):
                        shards._partition_validated(all_rows, selected, profile)
                    continue
                for timings in (None, {rows[0]["name"]: 0.25, "unknown-observation": 125.0}):
                    parts = self.relationship_oracle(all_rows, selected, profile, timings)
                    self.assertEqual(shards._partition_validated(all_rows, selected, profile, timings), parts)
                    for shard in shards.shard_ids(profile):
                        with self.subTest(profile=profile, selection=selection, timed=bool(timings), shard=shard):
                            expected = {**shards.evidence_for(rows, parts, profile, timings, roots, selection),
                                        "shard": shard}
                            with patch.object(shards, "inventory_roots", return_value=roots):
                                prefab = shards.plan(self.root, shard, profile, timings, selection)
                            self.assertEqual((self.root / "ctest.partition.json").read_bytes(),
                                             (json.dumps(expected, sort_keys=True) + os.linesep).encode())
                            positions = {row["name"]: index for index, row in enumerate(rows, 1)}
                            indices = [positions[name] for name in parts[shard - 1]]
                            self.assertEqual((self.root / "ctest.shard-index.txt").read_bytes(),
                                             (",".join(map(str, [indices[0], indices[0], 1, *indices[1:]])) + os.linesep).encode())
                            self.assertEqual(prefab, any(name.startswith("PrefabRuntimeTest.")
                                                        for name in parts[shard - 1]))
                    self.assertEqual({name: (self.root / name).read_bytes() for name in inputs}, inputs)
                    self.assertEqual((rows, all_rows), before)

    def test_public_string_subclass_hash_counts_and_refusals_stay_exact(self):
        class Name(str):
            calls = 0
            limit = None

            def __hash__(self):
                Name.calls += 1
                if Name.limit is not None and Name.calls >= Name.limit:
                    raise ValueError("bounded callback exhausted")
                return str.__hash__(self)

        for profile in ("gcc", "msvc", "ucrt64"):
            name = Name("Callback.Self")
            rows = self.tests + [test(name, DEPENDS=[name])]
            selected = [row["name"] for row in rows]
            for limit in (None, 20, 21, 23, 24):
                Name.calls, Name.limit = 0, limit
                with self.subTest(profile=profile, limit=limit):
                    if limit is None:
                        result = shards.partition(rows, selected, profile)
                        self.assertEqual(Name.calls, 24)
                        self.assertEqual(sorted(map(str, sum(result, []))), sorted(map(str, selected)))
                    else:
                        with self.assertRaisesRegex(ValueError, "bounded callback exhausted"):
                            shards.partition(rows, selected, profile)
                        self.assertEqual(Name.calls, limit)

    def test_patched_json_and_custom_timings_keep_generic_callbacks(self):
        class Name(str):
            calls = 0

            def __hash__(self):
                Name.calls += 1
                return str.__hash__(self)

        for custom in ("name", "timings"):
            observations = []
            for fallback in (True, False):
                rows = copy.deepcopy(self.tests)
                name = Name("Callback.Self") if custom == "name" else "Callback.Self"
                rows.append(test(name, DEPENDS=[name]))
                calls = []

                class Timings(dict):
                    def items(self):
                        calls.append("items")
                        rows[0]["unknown"] = "callback change"
                        return super().items()

                    def get(self, key, default=None):
                        calls.append(("get", key))
                        return super().get(key, default)

                timings = Timings({"Callback.Self": 1.0}) if custom == "timings" else None
                Name.calls = 0
                generic = shards._partition_validated
                with patch.object(shards, "read_json", side_effect=lambda path: {"tests": rows}), patch.object(
                        shards, "_partition_projected", side_effect=generic if fallback else shards._partition_projected):
                    shards.plan(self.root, 1, "gcc", timings)
                observations.append((Name.calls, calls, copy.deepcopy(rows),
                                     (self.root / "ctest.partition.json").read_bytes()))
            self.assertEqual(observations[0], observations[1])
            self.assertTrue(observations[0][0] if custom == "name" else observations[0][1])

    def test_projected_independent_selected_names_and_ignored_reader_values_keep_callbacks(self):
        for custom in ("selected-name", "ignored-value"):
            observations = []
            for fallback in (True, False):
                calls = []

                class Name(str):
                    def __hash__(self):
                        calls.append("hash")
                        return str.__hash__(self)

                class Ignored(list):
                    def __iter__(self):
                        calls.append("ignored iteration")
                        return super().__iter__()

                all_rows, selected_rows = copy.deepcopy(self.tests), copy.deepcopy(self.tests)
                if custom == "selected-name":
                    # Equality validation does not prove that names from the
                    # independently read selected graph have exact builtin type.
                    selected_rows[0]["name"] = Name(selected_rows[0]["name"])
                else:
                    all_rows[0]["unknown"] = Ignored(["literal"])
                    selected_rows[0]["unknown"] = Ignored(["literal"])
                reader = lambda path: {"tests": all_rows if path.name == "ctest.all.json" else selected_rows}
                generic = shards._partition_validated
                with patch.object(shards, "read_json", side_effect=reader), patch.object(
                        shards, "_partition_projected", side_effect=generic if fallback else shards._partition_projected):
                    shards.plan(self.root, 1)
                observations.append((list(calls), (self.root / "ctest.partition.json").read_bytes(),
                                     (self.root / "ctest.shard-index.txt").read_bytes()))
                self.assertIsNot(all_rows, selected_rows)
            self.assertTrue(observations[0][0])
            self.assertEqual(observations[0], observations[1])

    def test_projected_refusals_preserve_outputs_and_generic_error_order(self):
        self.write_plan(self.root, 1)
        outputs = {name: (self.root / name).read_bytes()
                   for name in ("ctest.partition.json", "ctest.shard-index.txt")}
        variants = [([], self.names, None), (self.tests, [], None),
                    (self.tests, self.names + self.names[:1], None),
                    (self.tests, ["unknown"], None),
                    (self.tests + [test("dependent", DEPENDS=["missing"])], self.names, {"bad": 0}),
                    (self.tests + [test("dependent", DEPENDS=[self.names[0]])], self.names, None)]
        variants.extend((self.tests, self.names, {self.names[0]: value})
                        for value in (0, -1, True, "1", float("nan"), float("inf")))
        for rows, selected, timings in variants:
            errors = []
            for projected in (False, True):
                try:
                    shards._partition_validated(rows, selected, "gcc", timings, _projected=projected)
                except (ValueError, KeyError, TypeError) as error:
                    errors.append((type(error), str(error)))
            self.assertEqual(len(errors), 2)
            self.assertEqual(errors[0], errors[1])
        # Shared validation refuses duplicate inventory names in every path;
        # valid inputs retain the same generic and projected semantics.
        for profile in shards.PROFILES:
            rows = self.tests + self.tests[:1]
            timings = None
            for projected in (False, True):
                with self.assertRaisesRegex(ValueError, "duplicate CTest names"):
                    shards._partition_validated(rows, self.names, profile, timings, _projected=projected)
            self.assertEqual(shards._partition_validated(self.tests, self.names, profile, timings),
                             shards._partition_validated(self.tests, self.names, profile, timings, _projected=True))
        for content in ("{", '{"tests": [], "tests": []}', '{"tests": []}'):
            (self.root / "ctest.all.json").write_text(content, encoding="utf-8")
            with self.assertRaises(ValueError):
                shards.plan(self.root, 1)
            self.assertEqual({name: (self.root / name).read_bytes() for name in outputs}, outputs)
        for error in (PermissionError("unreadable"), OSError("partial read")):
            with patch.object(Path, "read_text", side_effect=error), self.assertRaises(type(error)):
                shards.plan(self.root, 1)
            self.assertEqual({name: (self.root / name).read_bytes() for name in outputs}, outputs)

    def test_projected_canonicalization_work_is_linear_for_a_growing_component(self):
        # Bound projection overhead beyond shared joins and canonical-sort input
        # volume. Shared relationship validation retains its set-copy joins.
        # 128 connected rows plus 40 isolated rows keep both shards nonempty.
        count = 128
        rows = [test(f"Connected.Case{index}", DEPENDS=[f"Connected.Case{max(0, index - 1)}"])
                for index in range(count)] + self.tests
        for name in ("ctest.all.json", "ctest.inventory.json"):
            (self.root / name).write_text(json.dumps({"tests": rows}), encoding="utf-8")
        joins, sort_items = 0, 0
        original_trace = sys.gettrace()
        source = str(Path(shards.__file__).resolve())

        def trace(frame, event, arg):
            nonlocal joins
            if event == "line" and frame.f_code.co_filename == source and frame.f_code.co_name == "join":
                joins += 1
            return trace

        def sort(values, *args, **kwargs):
            nonlocal sort_items
            result = sorted(values, *args, **kwargs)
            if result and all(type(value) is str for value in result):
                sort_items += len(result)
            return result

        try:
            sys.settrace(trace)
            shards.relationship_groups(rows, [row["name"] for row in rows], "gcc")
            shared_joins = joins
            joins = 0
            with patch.object(shards, "sorted", new=sort, create=True):
                shards.plan(self.root, 1)
        finally:
            sys.settrace(original_trace)
        with self.subTest(work="joins"):
            self.assertGreater(shared_joins, 0)
            self.assertEqual(joins, shared_joins)
            self.assertLessEqual(joins - shared_joins, 12 * count)
        with self.subTest(work="canonicalization"):
            self.assertLessEqual(sort_items, 3 * len(rows))
        parts = shards.read_json(self.root / "ctest.partition.json")["partitions"]
        self.assertEqual(parts, self.relationship_oracle(rows, [row["name"] for row in rows], "gcc", None))

    def test_projected_shape_ignores_subtree_work_hooks_aliases_and_cycles(self):
        def refuse(*args, **kwargs):
            raise AssertionError("ignored value hook invoked")

        class Opaque(list):
            __iter__ = __len__ = __getitem__ = __bool__ = __eq__ = __hash__ = refuse

        class Meta(type):
            __eq__ = __hash__ = refuse

        class Scalar(metaclass=Meta):
            pass

        rows = copy.deepcopy(self.tests)
        rows[0]["properties"] = [{"name": "TIMEOUT", "value": None}]
        small = {"opaque": None}
        cycle = []
        cycle.append(cycle)
        large = {"opaque": [Opaque(), Scalar(), cycle, *({"nested": [0] * 8} for _ in range(4096))]}
        counts = []
        for ignored in (small, large):
            rows[0]["ignored"] = ignored
            rows[0]["properties"][0]["value"] = ignored
            checks = []

            def observe(value):
                checks.append(1)
                return type(value)

            with patch.object(shards, "type", new=observe, create=True):
                self.assertTrue(shards._grouping_shape(rows, self.names, "gcc", None))
            counts.append(len(checks))
            expected = shards._partition_validated(rows, self.names)
            with patch.object(shards, "_partition_validated", side_effect=AssertionError("unexpected fallback")):
                self.assertEqual(shards._partition_projected(rows, self.names), expected)
            self.assertIs(rows[0]["ignored"], ignored)
            self.assertEqual(rows[0]["command"], ["test-binary", self.names[0]])
            self.assertIs(rows[0]["properties"][0]["value"], ignored)
        self.assertGreater(counts[0], 0)
        self.assertEqual(counts[0], counts[1])
        self.assertIs(cycle[0], cycle)
        # Shared observed relation lists and property dictionaries are read-only.
        relation = {"name": "RESOURCE_LOCK", "value": ["shared"]}
        rows[0]["properties"] = [relation]
        rows[1]["properties"] = [relation]
        self.assertTrue(shards._grouping_shape(rows, self.names, "gcc", None))
        self.assertEqual(shards._partition_projected(rows, self.names),
                         shards._partition_validated(rows, self.names))
        self.assertIs(rows[0]["properties"][0], rows[1]["properties"][0])
        self.assertEqual(relation, {"name": "RESOURCE_LOCK", "value": ["shared"]})

    def test_projected_shape_rejects_observed_hooks_before_dict_access(self):
        calls = []

        def refuse(*args, **kwargs):
            calls.append("hook")
            raise ValueError("observed callback")

        class Mapping(dict):
            __iter__ = items = values = get = __getitem__ = refuse

        class Sequence(list):
            __iter__ = __len__ = __getitem__ = refuse

        class Name(str):
            __hash__ = __eq__ = refuse

        class Key(str):
            __hash__ = str.__hash__
            __eq__ = refuse

        class Number(float):
            __gt__ = refuse

        class Meta(type):
            __eq__ = __hash__ = refuse

        class Scalar(metaclass=Meta):
            pass

        variants = [
            (Sequence(), self.names, "gcc", None),
            ([Mapping()], self.names, "gcc", None),
            ([{"name": Name("name")}], self.names, "gcc", None),
            (self.tests, Sequence(), "gcc", None),
            (self.tests, [Name("name")], "gcc", None),
            (self.tests, self.names, Name("gcc"), None),
            (self.tests, self.names, "gcc", Mapping()),
            (self.tests, self.names, "gcc", {"unused": Number(1)}),
            (self.tests, [Scalar()], "gcc", None),
            (self.tests, self.names, "gcc", {"unused": Scalar()}),
            ([{**test("name"), "command": Sequence()}], ["name"], "gcc", None),
            ([{**test("name"), "command": [Name("test-binary")]}], ["name"], "gcc", None),
            ([test("name", LABELS=Sequence())], ["name"], "gcc", None),
            ([test("name", LABELS=[Name("manual")])], ["name"], "gcc", None),
            ([test("name", DEPENDS=Sequence())], ["name"], "gcc", None),
            ([test("name", DEPENDS=[Name("name")])], ["name"], "gcc", None),
            ([{"name": "name", "properties": [Mapping()]}], ["name"], "gcc", None),
            ([{"name": "name", "properties": [{"name": Name("DEPENDS")}]}], ["name"], "gcc", None),
            ([{Key("name"): "name"}], ["name"], "gcc", None),
            ([{"name": "name", "properties": [{Key("name"): "DEPENDS"}]}], ["name"], "gcc", None),
        ]
        calls.clear()
        for index, arguments in enumerate(variants):
            with self.subTest(shape=index):
                self.assertFalse(shards._grouping_shape(*arguments))
                self.assertEqual(calls, [])
                with patch.object(shards, "_partition_validated", return_value="generic") as generic:
                    self.assertEqual(shards._partition_projected(*arguments), "generic")
                    generic.assert_called_once_with(*arguments)
        # A key equal to the queried name must run its historical callback in
        # the generic path, after a callback-free gate has declined the shape.
        for rows in ([{Key("name"): "name"}],
                     [{"name": "name", "command": ["test-binary"],
                       "properties": [{Key("name"): "DEPENDS"}]}]):
            observations = []
            for operation in (shards._partition_validated, shards._partition_projected):
                calls.clear()
                with self.assertRaisesRegex(ValueError, "observed callback"):
                    operation(rows, ["name"])
                observations.append(list(calls))
            self.assertEqual(observations, [["hook"], ["hook"]])

    def test_projected_relevant_malformed_shapes_keep_generic_results_and_order(self):
        cycle = []
        cycle.append(cycle)
        variants = [None, [], [None], [{}], [{"name": "a", "properties": {}}],
                    [{"name": "a", "properties": [None]}],
                    [{"name": "a", "properties": [{}]}],
                    [test("a", DEPENDS="missing")], [test("a", DEPENDS=[])],
                    [test("a", DEPENDS=[None])], [test("a", DEPENDS=cycle)],
                    [{"name": "a", "properties": [{"name": "DEPENDS"}]}],
                    [test("a", DEPENDS=["missing"])]]
        for rows in variants:
            outcomes = []
            for operation in (shards._partition_validated, shards._partition_projected):
                try:
                    outcomes.append(("result", operation(rows, ["a"], "gcc", {"bad": 0})))
                except (ValueError, KeyError, TypeError) as error:
                    outcomes.append((type(error), str(error)))
            self.assertEqual(outcomes[0], outcomes[1])



class ExecutionNormalizationTests(unittest.TestCase):
    def test_plan_does_not_sort_empty_builtin_property_lists(self):
        case = CTestShardTests()
        case.setUp()
        self.addCleanup(case.doCleanups)
        rows = case.tests
        rows[0]["properties"] = [{"name": "TIMEOUT", "value": 2.5},
                                  {"name": "ENVIRONMENT", "value": ["A=one", "B=two"]}]
        del rows[1]["properties"]
        for name in ("ctest.all.json", "ctest.inventory.json"):
            (case.root / name).write_text(json.dumps({"tests": rows}), encoding="utf-8")
        roots = shards.inventory_roots(case.root)
        for profile in shards.PROFILES:
            parts = shards._partition_validated(rows, case.names, profile)
            expected = {**shards.evidence_for(rows, parts, profile, roots=roots), "shard": 1}
            calls = []

            def sort(values, *args, **kwargs):
                if type(values) is list and not values:
                    calls.append("empty sort")
                return sorted(values, *args, **kwargs)

            with self.subTest(profile=profile), patch.object(shards, "sorted", new=sort, create=True):
                shards.plan(case.root, 1, profile)
            self.assertEqual((case.root / "ctest.partition.json").read_bytes(),
                             (json.dumps(expected, sort_keys=True) + os.linesep).encode())
            self.assertEqual(calls, [])

    def test_normalization_keeps_custom_sequence_callbacks_and_error_order(self):
        events = []

        class Properties(list):
            def __iter__(self):
                events.append("iterate")
                return super().__iter__()

            def __len__(self):
                events.append("length")
                return super().__len__()

            def __bool__(self):
                raise AssertionError("custom truth callback must not run")

        class Row(dict):
            def items(self):
                events.append("items")
                return super().items()

            def get(self, *args):
                events.append("get")
                return super().get(*args)

        class Property(dict):
            def __getitem__(self, key):
                events.append(("property", key))
                return super().__getitem__(key)

        class Broken(Properties):
            def __iter__(self):
                events.append("broken iteration")
                raise ValueError("property read failed")

        def original(rows):
            result = [{**{key: value for key, value in row.items() if key != "backtrace"},
                       "properties": sorted(row.get("properties", []), key=lambda prop: prop["name"])}
                      for row in rows]
            json.dumps(result, allow_nan=False)
            return result

        for properties in (Properties(), Properties([Property(name="TIMEOUT", value=3)]), Broken()):
            rows = [Row(name="callback", command=["binary"], properties=properties, backtrace=1)]
            observations = []
            for operation in (original, shards.execution_inventory):
                events.clear()
                try:
                    result = operation(rows)
                    outcome = ("value", json.dumps(result))
                except ValueError as error:
                    outcome = (type(error), str(error))
                observations.append((outcome, list(events)))
            self.assertEqual(observations[0], observations[1])
            self.assertTrue(observations[0][1])

    def test_empty_normalization_stays_fresh_and_still_validates_every_value(self):
        shared = []
        rows = [test("first"), test("second")]
        for row in rows:
            row["properties"] = shared
        first = shards.execution_inventory(rows)
        second = shards.execution_inventory(rows)
        self.assertEqual(first, second)
        for index in (0, 1):
            self.assertIsNot(first[index]["properties"], shared)
            self.assertIsNot(first[index]["properties"], second[index]["properties"])
        self.assertIsNot(first[0]["properties"], first[1]["properties"])
        first[0]["properties"].append({"name": "TIMEOUT", "value": 1})
        self.assertEqual(shared, [])
        self.assertEqual(shards.execution_inventory([]), [])
        for invalid in (float("nan"), float("inf"), object()):
            rows[0]["unknown"] = invalid
            with self.subTest(invalid=type(invalid)), self.assertRaisesRegex(ValueError, "non-finite or invalid"):
                shards.execution_inventory(rows)
        cycle = []
        cycle.append(cycle)
        rows[0]["unknown"] = cycle
        with self.assertRaisesRegex(ValueError, "non-finite or invalid"):
            shards.execution_inventory(rows)


class CaseProgressResult(unittest.TextTestResult):
    @staticmethod
    def _clock():
        # Two scalar fields, exactly 22 ASCII bytes. Status: 0 valid, 1 missing,
        # 2 OS timer failure, 3 unsupported, 4 invalid scalar/range. No wall clock.
        timer = getattr(time, "monotonic_ns", None)
        if timer is None:
            return "1 00000000000000000000"
        try:
            value = timer()
        except PermissionError:
            raise
        except OSError:
            return "2 00000000000000000000"
        except NotImplementedError:
            return "3 00000000000000000000"
        if type(value) is not int or not 0 <= value < 10**20:
            return "4 00000000000000000000"
        return f"0 {value:020d}"

    @staticmethod
    def _phase(profile, selection, shard, phase):
        # Policy CLI observations use this fixed roster:
        # 8 profile + 24 * 14 lane records, <=67 ASCII bytes each with LF
        # (68 with CRLF), 20210/20554 bytes total. No graph values are emitted.
        # Synchronous stderr keeps its existing, unbounded OS write latency.
        print(f"PHASE {profile} {selection} {shard} {phase} {CaseProgressResult._clock()}",
              file=sys.stderr, flush=True)

    def startTest(self, case):
        super().startTest(case)
        print(f"\nCASE START {case.id()}\nCLOCK START {self._clock()}", file=sys.stderr, flush=True)

    def stopTest(self, case):
        # FINISH records completion, including failures, errors and skips.
        print(f"\nCASE FINISH {case.id()}\nCLOCK FINISH {self._clock()}", file=sys.stderr, flush=True)
        super().stopTest(case)


class CaseProgressRunner(unittest.TextTestRunner):
    resultclass = CaseProgressResult


class GateArgumentTests(unittest.TestCase):
    @staticmethod
    def arguments(**changes):
        values = dict(result="success", cpp="true", cancelled="false", event="pull_request",
                      draft="false", qualification="false", enabled="true", classification="success")
        values.update(changes)
        return [item for key, value in values.items() for item in ("--" + key, value)]

    @staticmethod
    def invoke(argv, **kwargs):
        import io

        parsers = []
        initialize = shards.argparse.ArgumentParser.__init__

        def observe(parser, *args, **options):
            initialize(parser, *args, **options)
            parsers.append(parser)

        stdout, stderr = io.StringIO(), io.StringIO()
        with patch.object(sys, "argv", argv), patch.object(sys, "stdout", stdout), patch.object(
                sys, "stderr", stderr), patch.object(shards.argparse.ArgumentParser, "__init__", observe):
            try:
                status = shards.main(**kwargs)
            except SystemExit as error:
                status = error.code
        return status, stdout.getvalue(), stderr.getvalue(), parsers

    def test_gate_retains_parser_order_and_only_configures_selected_actions(self):
        with patch.dict(os.environ):
            os.environ.pop("GITHUB_STEP_SUMMARY", None)
            status, stdout, stderr, parsers = self.invoke(["shards.py", "gate", *self.arguments()])
        self.assertEqual((status, stdout, stderr), (0, "route: sharded\n", ""))
        self.assertEqual([parser.prog for parser in parsers],
                         ["shards.py", "shards.py plan", "shards.py run", "shards.py merge",
                          "shards.py collect", "shards.py gate"])
        self.assertEqual([len(parser._actions) for parser in parsers], [2, 1, 1, 1, 1, 9])
        self.assertEqual(list(parsers[0]._actions[1].choices), ["plan", "run", "merge", "collect", "gate"])
        self.assertEqual([action.option_strings for action in parsers[-1]._actions],
                         [["-h", "--help"], *[[self.arguments()[index]] for index in range(0, 16, 2)]])

    def test_other_first_tokens_retain_complete_actions_and_profile_defaults(self):
        for profile in shards.PROFILES:
            for argv in (["shards.py"], ["shards.py", "--help", "gate"],
                         ["shards.py", "gate ", "--help"], ["shards.py", "gat", "--help"],
                         *[["shards.py", mode, "--help"] for mode in ("plan", "run", "merge", "collect")]):
                with self.subTest(profile=profile, argv=argv):
                    _, _, _, parsers = self.invoke(argv, default_profile=profile, legacy_evidence=True)
                    self.assertEqual([len(parser._actions) for parser in parsers], [2, 8, 3, 8, 5, 9])
                    for parser in parsers[1:4]:
                        self.assertEqual(parser.get_default("profile"), profile)
        with self.assertRaises(IndexError):
            self.invoke([])

    def test_gate_routes_and_refusals_preserve_summary_writes(self):
        rows = (({}, "sharded"), ({"qualification": "true"}, "full"),
                ({"cpp": "false"}, "python-only"), ({"enabled": "false", "result": "skipped"}, "skipped"),
                ({"classification": "failure"}, "path classification did not succeed"),
                ({"draft": "true"}, "draft or missing draft metadata"),
                ({"cancelled": "true"}, "workflow cancelled or cancellation status missing"),
                ({"enabled": "false"}, "unexpected producer on excluded route"),
                ({"result": "failure"}, "expected producers did not all succeed"),
                ({"cpp": "false", "qualification": "true"}, "qualification cannot use Python-only route"))
        with tempfile.TemporaryDirectory() as directory:
            for index, (changes, expected) in enumerate(rows):
                summary = Path(directory) / str(index)
                summary.write_bytes(b"sentinel\n")
                with self.subTest(changes=changes), patch.dict(os.environ, GITHUB_STEP_SUMMARY=str(summary)):
                    status, stdout, stderr, _ = self.invoke(["shards.py", "gate", *self.arguments(**changes)])
                success = index < 4
                self.assertEqual(status, 0 if success else 1)
                self.assertEqual(stdout, f"route: {expected}\n" if success else "")
                self.assertEqual(stderr, "" if success else f"CTest gate refused: {expected}\n")
                self.assertEqual(summary.read_bytes(), b"sentinel\n" +
                                 (f"route: {expected}{os.linesep}".encode() if success else b""))
            missing = Path(directory) / "absent" / "summary"
            with patch.dict(os.environ, GITHUB_STEP_SUMMARY=str(missing)):
                status, stdout, stderr, _ = self.invoke(["shards.py", "gate", *self.arguments()])
            self.assertEqual((status, stdout), (1, "route: sharded\n"))
            self.assertTrue(stderr.startswith("CTest gate refused: "))
            self.assertFalse(missing.exists())

    def test_gate_help_required_unknown_and_whitespace_program_names(self):
        for prog in ("shards.py", "  shards.py", "shards.py  "):
            status, stdout, stderr, _ = self.invoke([prog, "gate", "--help", "--help"])
            self.assertEqual((status, stderr), (0, ""))
            for option in self.arguments()[::2]:
                self.assertIn(option, stdout)
            for args, diagnostic in (([], "the following arguments are required:"),
                                     ([*self.arguments(), "--unknown"], "unrecognized arguments: --unknown")):
                with self.subTest(prog=prog, args=args):
                    status, stdout, stderr, _ = self.invoke([prog, "gate", *args])
                    self.assertEqual((status, stdout), (2, ""))
                    self.assertIn(diagnostic, stderr)


# Whole-case overrides balance the groups for the unchanged 30s Windows
# deadline. New methods in known classes enter part 1 automatically.
CTEST_SHARD_CLASS_PARTITIONS = {
    "CTestShardTests": "1",
    "ExecutionNormalizationTests": "1",
    "GateArgumentTests": "1",
    "CTestShardPartitionSelectionTests": "1",
}
CTEST_SHARD_CASE_PARTITIONS = {
    "CTestShardTests.test_policy_cli_publication_and_resolved_execution_preserve_fixture_contract": "2",
    "CTestShardTests.test_manifest_oracle_construction_is_bounded_with_real_plans": "5",
    "CTestShardTests.test_plan_manifest_bytes_match_general_evidence_in_every_profile": "3",
    "CTestShardTests.test_selected_junit_exact_coverage_and_selection_bound_digests_in_all_profiles": "3",
    "CTestShardTests.test_all_full_and_light_unions_preserve_floors_and_required_cases": "4",
    "CTestShardTests.test_projected_plans_match_traversal_oracle_and_literal_manifests": "4",
    "CTestShardTests.test_cli_independent_discovery_refuses_before_any_plan_or_output_write": "4",
    "CTestShardTests.test_execution_field_drift_refuses_discovery_run_and_merge_without_mutation": "8",
    "CTestShardTests.test_ctest_windows_reservations_run_alone_longest_first_with_smaller_parallelism": "5",
    "CTestShardTests.test_full_capture_prefab_output_survives_both_contract_suites": "5",
    "CTestShardTests.test_manual_fixture_and_dependency_edges_still_refuse_before_writing_every_plan": "5",
    "CTestShardTests.test_case_progress_records_survive_success_failure_and_interruption": "6",
    "CTestShardTests.test_floor_precedes_selection_and_refusal_does_not_replace_existing_plan": "6",
    "CTestShardTests.test_manual_gl_selection_reaches_shared_and_compatibility_cli_planners": "6",
    "CTestShardTests.test_light_refusal_fixture_is_constructed_once_with_real_boundaries": "7",
    "CTestShardTests.test_cli_output_absent_empty_unwritable_and_invalid_inventory": "7",
    "CTestShardTests.test_different_recorded_roots_refuse_merge_without_mutating_evidence": "7",
    "CTestShardTests.test_cli_policy_defaults_and_explicit_selection_use_full_inventory_floor": "8",
    "CTestShardTests.test_ctest_partial_discovery_without_target_list_preserves_selection": "8",
    "CTestShardTests.test_ctest_defined_invalid_target_lists_refuse_even_filtered": "8",
    "CTestShardTests.test_plan_validates_two_owned_graphs_and_keeps_fresh_refusals": "8",
    "CTestShardTests.test_light_coverage_refuses_missing_extra_skipped_or_mismatched_selection_without_mutation": "8",
    "CTestShardTests.test_every_lane_rechecks_same_context_and_preserves_limits_and_exit_status": "8",
}


CTEST_SHARD_PARTITIONS = tuple(sorted(set(CTEST_SHARD_CLASS_PARTITIONS.values()) |
                                     set(CTEST_SHARD_CASE_PARTITIONS.values()), key=int))


def ctest_shard_cases(suite):
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from ctest_shard_cases(item)
        else:
            yield item


def ctest_shard_partition_suite(loader, module, partition):
    if partition not in CTEST_SHARD_PARTITIONS:
        raise ValueError("unknown CTest shard partition")
    if any(value not in CTEST_SHARD_PARTITIONS for value in (
            *CTEST_SHARD_CLASS_PARTITIONS.values(), *CTEST_SHARD_CASE_PARTITIONS.values())):
        raise ValueError("invalid CTest shard partition mapping")
    # Scan the finite unittest roster once; selection starts no subprocesses.
    cases = list(ctest_shard_cases(loader.loadTestsFromModule(module)))
    seen = set()
    selected = []
    for case in cases:
        key = f"{type(case).__name__}.{case._testMethodName}"
        if key in seen:
            raise ValueError("duplicate CTest shard case: " + key)
        seen.add(key)
        class_partition = CTEST_SHARD_CLASS_PARTITIONS.get(type(case).__name__)
        if class_partition is None:
            raise ValueError("unregistered CTest shard class: " + type(case).__name__)
        if CTEST_SHARD_CASE_PARTITIONS.get(key, class_partition) == partition:
            selected.append(case)
    if not CTEST_SHARD_CASE_PARTITIONS.keys() <= seen:
        raise ValueError("missing dedicated CTest shard case")
    if not selected:
        raise ValueError("empty CTest shard partition")
    return unittest.TestSuite(selected)


def ctest_shard_main(argv=None):
    # Without a partition, retain unittest's complete CLI and discovery semantics.
    parser = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
    parser.add_argument("--partition", choices=CTEST_SHARD_PARTITIONS, action="append")
    parser.add_argument("--list-tests", action="store_true")
    args, remaining = parser.parse_known_args(argv)
    if args.partition is None:
        if args.list_tests:
            parser.error("--list-tests requires --partition")
        unittest.main(argv=[sys.argv[0], *remaining], testLoader=unittest.TestLoader(),
                      testRunner=CaseProgressRunner)
        return 0
    if len(args.partition) != 1 or remaining:
        parser.error("one partition is required, without unittest selectors")
    try:
        suite = ctest_shard_partition_suite(unittest.TestLoader(), sys.modules[__name__], args.partition[0])
    except ValueError as error:
        parser.error(str(error))
    if args.list_tests:
        for case in ctest_shard_cases(suite):
            print(case.id())
        return 0
    result = CaseProgressRunner(verbosity=2).run(suite)
    return int(not result.wasSuccessful())


class CTestShardPartitionSelectionTests(unittest.TestCase):
    def test_partitions_cover_discovered_old_and_new_cases_exactly_once(self):
        module = sys.modules[__name__]
        loader = unittest.TestLoader()
        complete = [case.id() for case in ctest_shard_cases(loader.loadTestsFromModule(module))]
        selected = []
        for partition in CTEST_SHARD_PARTITIONS:
            cases = list(ctest_shard_cases(ctest_shard_partition_suite(loader, module, partition)))
            self.assertTrue(cases)
            selected.extend(case.id() for case in cases)
        self.assertEqual(len(selected), len(set(selected)))
        self.assertCountEqual(selected, complete)
        self.assertTrue(any("CTestShardPartitionSelectionTests." in name for name in selected))
        self.assertEqual(set(CTEST_SHARD_CLASS_PARTITIONS),
                         {type(case).__name__ for case in ctest_shard_cases(loader.loadTestsFromModule(module))})
        # New methods in known classes enter selection without maintaining lists.
        class Extended(CTestShardTests):
            def test_added_case(self):
                pass
        Extended.__name__ = "CTestShardTests"
        extra = Extended("test_added_case")
        augmented = unittest.TestSuite([loader.loadTestsFromModule(module), extra])
        with patch.object(loader, "loadTestsFromModule", return_value=augmented):
            cases = list(ctest_shard_cases(ctest_shard_partition_suite(loader, module, "1")))
        self.assertIn(extra, cases)

    def test_selection_refuses_invalid_empty_duplicate_missing_and_unknown_rosters(self):
        module = sys.modules[__name__]
        loader = unittest.TestLoader()
        for invalid in ("", "0", "-1", str(max(map(int, CTEST_SHARD_PARTITIONS)) + 1), "01", "1/8", None, 1):
            with self.subTest(partition=invalid), self.assertRaises(ValueError):
                ctest_shard_partition_suite(loader, module, invalid)
        complete = list(ctest_shard_cases(loader.loadTestsFromModule(module)))
        for mapping in (CTEST_SHARD_CLASS_PARTITIONS, CTEST_SHARD_CASE_PARTITIONS):
            with patch.dict(mapping, {next(iter(mapping)): "unknown"}):
                with self.assertRaisesRegex(ValueError, "invalid CTest shard partition mapping"):
                    ctest_shard_partition_suite(loader, module, "1")
        dedicated = next(case for case in complete if type(case).__name__ + "." + case._testMethodName in CTEST_SHARD_CASE_PARTITIONS)
        class Unknown(unittest.TestCase):
            def test_unknown(self):
                pass
        for cases, partition, message in (
            ([], "1", "missing dedicated"),
            ([*complete, complete[0]], "1", "duplicate"),
            ([case for case in complete if case is not dedicated], "1", "missing dedicated"),
            ([*complete, Unknown("test_unknown")], "1", "unregistered"),
            ([case for case in complete if type(case).__name__ + "." + case._testMethodName
              in CTEST_SHARD_CASE_PARTITIONS], "1", "empty"),
        ):
            with self.subTest(message=message), patch.object(loader, "loadTestsFromModule", return_value=unittest.TestSuite(cases)):
                with self.assertRaisesRegex(ValueError, message):
                    ctest_shard_partition_suite(loader, module, partition)

    def test_measured_costly_cases_keep_their_balanced_groups(self):
        module = sys.modules[__name__]
        for partition in CTEST_SHARD_PARTITIONS[1:]:
            cases = ctest_shard_cases(ctest_shard_partition_suite(unittest.TestLoader(), module, partition))
            self.assertCountEqual(
                [type(case).__name__ + "." + case._testMethodName for case in cases],
                [key for key, value in CTEST_SHARD_CASE_PARTITIONS.items() if value == partition])

    def test_deadline_groups_select_the_measured_whole_cases(self):
        expected = {
            "5": (
                "test_ctest_windows_reservations_run_alone_longest_first_with_smaller_parallelism",
                "test_full_capture_prefab_output_survives_both_contract_suites",
                "test_manifest_oracle_construction_is_bounded_with_real_plans",
                "test_manual_fixture_and_dependency_edges_still_refuse_before_writing_every_plan",
            ),
            "6": (
                "test_case_progress_records_survive_success_failure_and_interruption",
                "test_floor_precedes_selection_and_refusal_does_not_replace_existing_plan",
                "test_manual_gl_selection_reaches_shared_and_compatibility_cli_planners",
            ),
            "7": (
                "test_cli_output_absent_empty_unwritable_and_invalid_inventory",
                "test_different_recorded_roots_refuse_merge_without_mutating_evidence",
                "test_light_refusal_fixture_is_constructed_once_with_real_boundaries",
            ),
            "8": (
                "test_cli_policy_defaults_and_explicit_selection_use_full_inventory_floor",
                "test_ctest_defined_invalid_target_lists_refuse_even_filtered",
                "test_ctest_partial_discovery_without_target_list_preserves_selection",
                "test_every_lane_rechecks_same_context_and_preserves_limits_and_exit_status",
                "test_execution_field_drift_refuses_discovery_run_and_merge_without_mutation",
                "test_light_coverage_refuses_missing_extra_skipped_or_mismatched_selection_without_mutation",
                "test_plan_validates_two_owned_graphs_and_keeps_fresh_refusals",
            ),
        }
        for partition, methods in expected.items():
            with self.subTest(partition=partition):
                cases = ctest_shard_cases(ctest_shard_partition_suite(
                    unittest.TestLoader(), sys.modules[__name__], partition))
                self.assertCountEqual(
                    [(type(case).__name__, case._testMethodName) for case in cases],
                    [("CTestShardTests", method) for method in methods])

    def test_cmake_registers_every_partition_once_with_unchanged_deadlines(self):
        cmake = (ROOT / "tools/CMakeLists.txt").read_text(encoding="utf-8")
        registrations = re.findall(r"add_test\(\s*NAME ([\w]+)\s+COMMAND ([^)]*)\)", cmake)
        actual = [(name, " ".join(command.split())) for name, command in registrations
                  if "test_ctest_shards.py" in command]
        expected = []
        for partition in CTEST_SHARD_PARTITIONS:
            name = "CTestShardContractPart" + partition
            expected.append((name, '"${Python3_EXECUTABLE}" -I -B '
                             '"${CMAKE_CURRENT_SOURCE_DIR}/ci/test_ctest_shards.py" --partition ' + partition))
            properties = re.findall(r"set_tests_properties\(" + name + r"\s+PROPERTIES ([^)]*)\)", cmake)
            self.assertEqual([" ".join(item.split()) for item in properties], ['LABELS "ci;tools" TIMEOUT 30'])
        self.assertCountEqual(actual, expected)
        self.assertEqual(len(actual), len(set(actual)))

    def test_direct_discovery_and_path_routes_keep_complete_suite(self):
        spec = importlib.util.spec_from_file_location("ci_paths", ROOT / "tools/ci/ci_paths.py")
        routes = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(routes)
        path = "tools/ci/test_ctest_shards.py"
        self.assertEqual(routes.PYTHON_TESTS.count(path), 1)
        self.assertEqual(routes.python_test_commands(ROOT).count((path,)), 1)
        for changed in (path, "tools/CMakeLists.txt"):
            self.assertTrue(routes.needs_cpp(changed))
        self.assertTrue(routes.needs_python(path))
        workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        self.assertRegex(workflow, r"(?m)^          python3 -I -B tools/ci/test_ctest_shards.py$")
        self.assertNotRegex(workflow, r"test_ctest_shards.py[^\n]*--partition")
        discovered = unittest.TestLoader().discover(str(ROOT / "tools/ci"), pattern="test_ctest_shards.py")
        expected = unittest.TestLoader().loadTestsFromModule(sys.modules[__name__])
        def roster(suite):
            return [(type(case).__name__, case._testMethodName) for case in ctest_shard_cases(suite)]
        self.assertCountEqual(roster(discovered), roster(expected))

    def test_cli_listing_and_refusals_use_actual_selection(self):
        for partition in CTEST_SHARD_PARTITIONS:
            with self.subTest(partition=partition), patch.object(sys, "stdout", new_callable=io.StringIO) as output, \
                    patch.object(unittest.defaultTestLoader, "testNamePatterns", ["*hidden_case*"]):
                self.assertEqual(ctest_shard_main(["--partition", partition, "--list-tests"]), 0)
                expected = ctest_shard_partition_suite(unittest.TestLoader(), sys.modules[__name__], partition)
                self.assertEqual(output.getvalue().splitlines(), [case.id() for case in ctest_shard_cases(expected)])
        for argv in (["--partition"], ["--partition", str(max(map(int, CTEST_SHARD_PARTITIONS)) + 1)], ["--partition", "01"],
                     ["--partition", "1", "--partition", "2"], ["--list-tests"],
                     ["--partition", "1", "CTestShardTests"], ["--partition", "1", "--unknown"]):
            with self.subTest(argv=argv), patch.object(sys, "stderr", new_callable=io.StringIO), self.assertRaises(SystemExit) as error:
                ctest_shard_main(argv)
            self.assertEqual(error.exception.code, 2)
        with patch.object(sys.modules[__name__], "ctest_shard_partition_suite", side_effect=ValueError("bad roster")), \
                patch.object(sys, "stderr", new_callable=io.StringIO) as output, self.assertRaises(SystemExit) as error:
            ctest_shard_main(["--partition", "1"])
        self.assertEqual(error.exception.code, 2)
        self.assertIn("bad roster", output.getvalue())
        with patch.object(unittest, "main") as run:
            self.assertEqual(ctest_shard_main([]), 0)
            run.assert_called_once_with(argv=[sys.argv[0]], testLoader=run.call_args.kwargs["testLoader"],
                                        testRunner=CaseProgressRunner)
            self.assertIsInstance(run.call_args.kwargs["testLoader"], unittest.TestLoader)
            self.assertIsNot(run.call_args.kwargs["testLoader"], unittest.defaultTestLoader)
        with patch.object(unittest, "main") as run:
            self.assertEqual(ctest_shard_main(["CTestShardTests", "-v"]), 0)
            run.assert_called_once_with(argv=[sys.argv[0], "CTestShardTests", "-v"],
                                        testLoader=run.call_args.kwargs["testLoader"], testRunner=CaseProgressRunner)

    def test_partition_runner_propagates_case_failure_and_reports_duration(self):
        class Fails(unittest.TestCase):
            def test_failure(self):
                self.fail("partition failure")
        class Passes(unittest.TestCase):
            def test_success(self):
                pass
        for cases, status in (([Fails("test_failure")], 1), ([Passes("test_success")], 0), ([], 0)):
            suite = unittest.TestSuite(cases)
            with self.subTest(status=status), patch.object(sys, "stderr", new_callable=io.StringIO) as output, patch.object(
                    sys.modules[__name__], "ctest_shard_partition_suite", return_value=suite):
                self.assertEqual(ctest_shard_main(["--partition", "1"]), status)
            self.assertEqual(output.getvalue().count("CASE START "), len(cases))
            self.assertEqual(output.getvalue().count("CASE FINISH "), len(cases))
            self.assertIn("Ran " + str(len(cases)) + " test", output.getvalue())
            if status:
                self.assertIn("partition failure", output.getvalue())

    def test_qualified_import_keeps_complete_partition_roster_and_listing(self):
        name = "ctest_shard_selection.qualified"
        spec = importlib.util.spec_from_file_location(name, Path(__file__))
        module = importlib.util.module_from_spec(spec)
        with patch.dict(sys.modules, {name: module}):
            spec.loader.exec_module(module)
            complete = list(module.ctest_shard_cases(unittest.TestLoader().loadTestsFromModule(module)))
            selected = []
            for partition in module.CTEST_SHARD_PARTITIONS:
                selected.extend(module.ctest_shard_cases(module.ctest_shard_partition_suite(unittest.TestLoader(), module, partition)))
            self.assertEqual(len(selected), len({case.id() for case in selected}))
            self.assertCountEqual([case.id() for case in selected], [case.id() for case in complete])
            expected = ctest_shard_cases(unittest.TestLoader().loadTestsFromModule(sys.modules[__name__]))
            self.assertCountEqual([(type(case).__name__, case._testMethodName) for case in complete],
                                  [(type(case).__name__, case._testMethodName) for case in expected])
            with patch.object(sys, "stdout", new_callable=io.StringIO) as output:
                self.assertEqual(module.ctest_shard_main(["--partition", "1", "--list-tests"]), 0)
            part_one = module.ctest_shard_partition_suite(unittest.TestLoader(), module, "1")
            self.assertEqual(output.getvalue().splitlines(), [case.id() for case in module.ctest_shard_cases(part_one)])


if __name__ == "__main__":
    raise SystemExit(ctest_shard_main())
