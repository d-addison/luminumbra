#!/usr/bin/env python3
"""Offline regressions for shared CTest partitions and aggregate evidence."""

import importlib.util
import copy
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

SPEC = importlib.util.spec_from_file_location("ctest_shards", Path(__file__).with_name("ctest_shards.py"))
shards = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(shards)


def test(name, **properties):
    return {"name": name, "command": ["test-binary", name], "properties": [{"name": key, "value": value} for key, value in properties.items()]}


class CTestShardTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.tests = [test(f"Suite.Case{index}") for index in range(40)]
        self.names = [row["name"] for row in self.tests]

    def write_plan(self, directory, shard, tests=None, all_tests=None, profile="gcc", roots=None, selection=None):
        directory.mkdir(parents=True, exist_ok=True)
        tests = self.tests if tests is None else tests
        for file, rows in (("ctest.all.json", all_tests if all_tests is not None else tests),
                           ("ctest.inventory.json", tests)):
            (directory / file).write_text(json.dumps({"tests": rows}), encoding="utf-8")
        # Artifact directories model producers with identical hosted checkout paths.
        roots = shards.inventory_roots(self.root) if roots is None else roots
        with patch.object(shards, "inventory_roots", return_value=roots):
            shards.plan(directory, shard, profile, selection=selection)
        return shards.read_json(directory / "ctest.partition.json")

    def reports(self, full=False, profile="gcc"):
        for shard in ((0,) if full else (1, 2)):
            directory = self.root / f"shard-{shard}"
            manifest = self.write_plan(directory, shard, profile=profile)
            root = ET.Element("testsuite", failures="0", errors="0", skipped="0")
            for name in manifest["partitions"][0 if full else shard - 1]:
                case = ET.SubElement(root, "testcase", name=name, status="run")
                ET.SubElement(case, "system-out").text = "receipt: " + name
            ET.ElementTree(root).write(directory / "ctest.junit.xml")

    def merge(self, full=False, profile="gcc"):
        shards.merge(self.root, self.root / "merged.xml", full=full, profile=profile)

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
        for shard in (1, 2):
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
                for shard in ((0,) if full else (1, 2)):
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
            yield "quoted-or-nested-assignment-case", changed
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
            yield "literal-root-case-" + key, changed
        for key, value in (("ENVIRONMENT", ["MODE=changed"]),
                           ("ENVIRONMENT_MODIFICATION", ["MODE=set:changed"]),
                           ("WORKING_DIRECTORY", "/opt/alternate-work"),
                           ("WILL_FAIL", True), ("PASS_REGULAR_EXPRESSION", ["changed"]),
                           ("FAIL_REGULAR_EXPRESSION", ["changed"]), ("SKIP_RETURN_CODE", 42),
                           ("SKIP_REGULAR_EXPRESSION", ["changed"]), ("TIMEOUT", 1),
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
        for profile in shards.PROFILES:
            self.write_plan(self.root, 1)
            shards.plan(self.root, 1, profile)
            before = {name: (self.root / name).read_bytes() for name in
                      ("ctest.partition.json", "ctest.shard-index.txt")}
            for change, tests in self.execution_changes():
                with self.subTest(profile=profile, change=change):
                    (self.root / "ctest.all.json").write_text(json.dumps({"tests": tests}))
                    with self.assertRaisesRegex(ValueError, "execution inventory differs"):
                        shards.plan(self.root, 1, profile)
                    response = subprocess.CompletedProcess([], 0, stdout=json.dumps({"tests": tests}))
                    with patch.object(shards.subprocess, "run", return_value=response) as invoke:
                        with self.assertRaisesRegex(ValueError, "execution inventory differs"):
                            shards.run(self.root, profile)
                        invoke.assert_called_once()
                    for name, content in before.items():
                        self.assertEqual((self.root / name).read_bytes(), content)
                    self.reports(profile=profile)
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

    def test_different_recorded_roots_refuse_merge_without_mutating_evidence(self):
        original = {"source": "C:/checkout", "build": "C:/checkout/build/release"}
        for kind in ("source", "build"):
            for root in (original[kind].upper(), original[kind].replace("/", "\\"),
                         original[kind] + "/", "D:/other", "/work/other"):
                with self.subTest(kind=kind, root=root):
                    self.reports()
                    manifests = []
                    for shard in (1, 2):
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
        for change in ({"roots": {**manifest["roots"], "source": "/opt/foreign"}},
                       {"version": 1}, {"version": 2}, {"version": 3}):
            path.write_text(json.dumps({**manifest, **change}))
            with patch.object(shards.subprocess, "run", return_value=response) as invoke:
                with self.assertRaisesRegex(ValueError, "digest differs"):
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
            preset, parallel, timeout = expected[profile]
            self.write_plan(self.root, 1)
            shards.plan(self.root, 1, profile=profile)
            response = subprocess.CompletedProcess([], 0, stdout=json.dumps({"tests": self.tests}))
            with self.subTest(profile=profile), patch.object(shards.subprocess, "run", side_effect=[response, subprocess.CompletedProcess([], 8)]) as invoke:
                self.assertEqual(shards.run(self.root, profile), 8)
                check, execute = invoke.call_args_list
                base = ["ctest", "--preset", preset, "--test-dir", str(self.root), "-LE", "manual"]
                self.assertEqual(check.args[0], [*base, "--show-only=json-v1"])
                self.assertEqual(execute.args[0], [*base, "--no-tests=error", "-I", str(self.root / "ctest.shard-index.txt"),
                                                  "--parallel", str(parallel), "--timeout", str(timeout),
                                                  "--output-junit", "ctest.junit.xml"])

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
                                    (["--shard", "3"], 2)):
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
        for shard in ((0,) if full else (1, 2)):
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
                    parts = shards.partition(tests, selection, profile)
                    shards.validate_union(selection, parts)

    def test_all_full_and_light_unions_preserve_floors_and_required_cases(self):
        for profile, filename in (("asan", "asan.yml"), ("gcc", "ci.yml"),
                                  ("msvc", "ci.yml"), ("ucrt64", "ci.yml")):
            workflow = (Path(__file__).resolve().parents[2] / ".github/workflows" / filename).read_text(encoding="utf-8")
            command = next(line for line in workflow.splitlines()
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
                    for shard in (1, 2):
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

    def test_ctest_loads_heavy_hook_preserving_labels_and_quick_cases(self):
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
        names = [*heavy, *quick]
        # Model GoogleTest's generated discovery file, including its TEST_LIST.
        discovery = "\n".join(
            f'add_test([==[{name}]==] [==[{Path(sys.executable).as_posix()}]==])\n'
            f'set_tests_properties([==[{name}]==] PROPERTIES TIMEOUT 1800)'
            for name in names
        )
        discovery += (
            f'\nset_tests_properties([==[{heavy[0]}]==] PROPERTIES LABELS "parity;simulation")\n'
            f'set_tests_properties([==[{quick[0]}]==] PROPERTIES LABELS "quick;refusal")\n'
            'set(LUMINUMBRA_WORLD_OPEN_TESTS ' + " ".join(f'[==[{name}]==]' for name in names) + ')\n'
        )
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
        for filters, selected in (((), names), (("-L", "heavy"), heavy), (("-LE", "heavy"), quick)):
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
                    self.assertEqual(row["command"], original["command"])
                    self.assertEqual(sorted(row["properties"], key=lambda prop: prop["name"]),
                                     sorted(original["properties"], key=lambda prop: prop["name"]))
        for name in heavy:
            with self.subTest(renamed=name):
                discovered.write_text(discovery.replace(name, name + "Renamed"), encoding="utf-8")
                result = discover()
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(f"Reviewed heavy correctness case missing: {name}", " ".join(result.stderr.split()))

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
        for mutation in ("missing", "extra", "skipped", "selection", "digest", "mixed", "floor"):
            self.policy_reports("light")
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
        for shard in (1, 2):
            (self.root / f"prefix-shard-{shard}-attempt-1").mkdir()
        before = sorted(path.name for path in self.root.iterdir())
        with patch.object(shards.shutil, "copytree", side_effect=OSError("copy failed")), self.assertRaises(OSError):
            shards.collect(self.root, "prefix", 1)
        self.assertEqual(sorted(path.name for path in self.root.iterdir()), before)

    def test_collect_move_failure_rolls_back_exposed_evidence(self):
        for shard in (1, 2):
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


if __name__ == "__main__":
    unittest.main()
