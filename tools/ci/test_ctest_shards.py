#!/usr/bin/env python3
"""Offline regressions for shared CTest partitions and aggregate evidence."""

import importlib.util
import copy
import json
import os
from pathlib import Path
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
            for shard in ((1, 2) if profile == "asan" else (0, 1, 2)):
                with self.subTest(profile=profile, shard=shard):
                    output.write_bytes(sentinel)
                    with patch.dict(os.environ, GITHUB_OUTPUT=str(output)):
                        self.write_plan(self.root, shard, tests=tests, profile=profile)
                    self.assertEqual(output.read_bytes(), sentinel)

    def test_cli_publishes_only_the_selected_real_plan(self):
        tests = [*self.tests, test("PrefabRuntimeTest.Installed"), test("PrefabDigest.Content"),
                 test("SimBudgetWorldParity.First"), test("SimBudgetWorldParity.Second")]
        output = self.root / "step-output"
        sentinel = b"sentinel=retained\n"
        for script, profile in (("asan_shards.py", "asan"), ("ctest_shards.py", "asan"),
                                ("ctest_shards.py", "gcc"), ("ctest_shards.py", "msvc"),
                                ("ctest_shards.py", "ucrt64")):
            for shard in ((1, 2) if profile == "asan" else (0, 1, 2)):
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
        tests = [*self.tests, test("PrefabRuntimeTest.Installed"), test("PrefabDigest.Content")]
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
        self.write_plan(self.root, 0)
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

    def write_plan(self, directory, shard, tests=None, all_tests=None, profile="gcc", roots=None):
        directory.mkdir(parents=True, exist_ok=True)
        tests = self.tests if tests is None else tests
        for file, rows in (("ctest.all.json", all_tests if all_tests is not None else tests),
                           ("ctest.inventory.json", tests)):
            (directory / file).write_text(json.dumps({"tests": rows}), encoding="utf-8")
        # Artifact directories model producers with identical hosted checkout paths.
        roots = shards.inventory_roots(self.root) if roots is None else roots
        with patch.object(shards, "inventory_roots", return_value=roots):
            shards.plan(directory, shard, profile)
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
        for change, error in (({"roots": {**manifest["roots"], "source": "/opt/foreign"}}, "roots differ"),
                              ({"version": 1}, "digest differs"), ({"version": 2}, "digest differs")):
            path.write_text(json.dumps({**manifest, **change}))
            with patch.object(shards.subprocess, "run", return_value=response) as invoke:
                with self.assertRaisesRegex(ValueError, error):
                    shards.run(self.root)
                invoke.assert_called_once()
        self.reports()
        path = self.root / "shard-2/ctest.partition.json"
        manifest = shards.read_json(path)
        for version in (1, 2):
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
        for status in (0, 8):
            response = subprocess.CompletedProcess([], 0, stdout=json.dumps({"tests": self.tests}))
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
                                    (["--shard", "3"], 2)):
            result = subprocess.run([sys.executable, "-I", "-B", script, "plan", "--directory", str(self.root),
                                     *arguments], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
            self.assertFalse((self.root / "ctest.partition.json").exists())
            self.assertFalse((self.root / "ctest.shard-index.txt").exists())

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
