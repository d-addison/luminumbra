#!/usr/bin/env python3
"""Offline ASan partition and exact JUnit union regressions."""

import importlib.util
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET


SPEC = importlib.util.spec_from_file_location("asan_shards", Path(__file__).with_name("asan_shards.py"))
shards = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(shards)


def test(name, **properties):
    return {"name": name, "properties": [{"name": key, "value": value} for key, value in properties.items()]}


def aggregate_inventory_arguments():
    workflow = Path(__file__).resolve().parents[2] / ".github/workflows/asan.yml"
    command = next(line for line in workflow.read_text(encoding="utf-8").splitlines()
                   if line.strip().startswith("python3 tools/ci/check_ctest_junit.py "))
    return shlex.split(command)[3:]


class AsanShardTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.names = ["ordinary", *(f"ActiveRegions/SimBudgetWorldParity.Replay/{n}" for n in range(4)), "last"]
        self.tests = [test(name) for name in self.names]

    def test_stride_alternates_four_heavy_tests_and_covers_exactly_once(self):
        result = shards.partition(self.tests, self.names)
        self.assertEqual(result, [[self.names[n - 1] for n in part] for part in ([1, 2, 4], [3, 5, 6])])
        # Heavy tests have their own alternating assignment regardless of start index.
        self.assertEqual([sum("SimBudgetWorldParity." in name for name in part)
                          for part in result], [2, 2])
        self.assertEqual(sorted(sum(result, [])), sorted(self.names))
        self.assertEqual(shards.partition(self.tests, self.names), result)

    def test_current_two_parameters_split_one_each(self):
        result = shards.partition(self.tests[:3], self.names[:3])
        self.assertEqual(result, [self.names[:2], self.names[2:3]])

    def test_fixture_setup_cleanup_and_transitive_consumers_stay_together(self):
        tests = self.tests + [test("setup", FIXTURES_SETUP=["art"]),
                              test("consumer", FIXTURES_REQUIRED=["art"], FIXTURES_SETUP=["scene"]),
                              test("render", FIXTURES_REQUIRED=["scene"]),
                              test("cleanup", FIXTURES_CLEANUP=["art"])]
        names = [row["name"] for row in tests]
        result = shards.partition(tests, names)
        owners = [part for part in result if "setup" in part]
        self.assertEqual(len(owners), 1)
        self.assertTrue({"setup", "consumer", "render", "cleanup"} <= set(owners[0]))
        self.assertEqual(sorted(sum(result, [])), sorted(names))

    def joined_fixture_chains(self, reverse_groups, conflicting_heavy=False):
        first = [test(self.names[1] if conflicting_heavy else "setup", FIXTURES_SETUP=["ab"]),
                 test("consumer", FIXTURES_REQUIRED=["ab"], FIXTURES_SETUP=["bc"])]
        second = [test(self.names[2], FIXTURES_CLEANUP=["cd"]),
                  test("other-setup", FIXTURES_SETUP=["cd"], FIXTURES_REQUIRED=["bc"])]
        # Form A-B and C-D before the final B-C join in either registration order.
        chains = second + first if reverse_groups else first + second
        prefix = [] if conflicting_heavy else [test(self.names[1]), test("padding")]
        return prefix + chains

    def test_three_chain_join_keeps_every_member_with_heavy_test_in_both_orders(self):
        for reverse_groups in (False, True):
            with self.subTest(reverse_groups=reverse_groups):
                tests = self.joined_fixture_chains(reverse_groups)
                names = [row["name"] for row in tests]
                selected = self.planned_selection(tests, tests)
                group = {"setup", "consumer", "other-setup", self.names[2]}
                # The early ordinary members would choose shard 0 by registration
                # order; the second heavy test forces the entire joined group to 1.
                self.assertEqual(selected, [[self.names[1]],
                                            [name for name in names if name != self.names[1]]])
                self.assertTrue(group <= set(selected[1]))
                self.assertEqual(shards.partition(tests, names), selected)

    def test_three_chain_join_refuses_conflicting_heavy_owners_before_writing(self):
        for reverse_groups in (False, True):
            for shard in (1, 2):
                with self.subTest(reverse_groups=reverse_groups, shard=shard):
                    tests = self.joined_fixture_chains(reverse_groups, conflicting_heavy=True)
                    for filename in ("ctest.all.json", "ctest.inventory.json"):
                        (self.root / filename).write_text(json.dumps({"tests": tests}), encoding="utf-8")
                    selection = self.root / "ctest.shard-index.txt"
                    selection.write_text("previous selection\n", encoding="utf-8")
                    with self.assertRaisesRegex(ValueError, "prevents balanced"):
                        shards.plan(self.root, shard)
                    self.assertEqual(selection.read_text(encoding="utf-8"), "previous selection\n")

    def planned_selection(self, all_tests, eligible):
        (self.root / "ctest.all.json").write_text(json.dumps({"tests": all_tests}))
        (self.root / "ctest.inventory.json").write_text(json.dumps({"tests": eligible}))
        selected = []
        # Model CTest 3.28: apply -LE manual first, then interpret -I against
        # that filtered roster, preserving registration order.
        filtered = [row["name"] for row in all_tests
                    if not any(prop["name"] == "LABELS" and
                               any("manual" in label for label in prop["value"])
                               for prop in row.get("properties", []))]
        for shard in (1, 2):
            shards.plan(self.root, shard)
            start, end, stride, *extra = map(int, (self.root / "ctest.shard-index.txt").read_text().split(","))
            indices = [*range(start, end + 1, stride), *extra]
            self.assertEqual(len(indices), len(set(indices)))
            selected.append([name for index, name in enumerate(filtered, 1) if index in indices])
        self.assertTrue(all(selected))
        self.assertFalse(set(selected[0]) & set(selected[1]))
        self.assertEqual(sorted(sum(selected, [])), sorted(row["name"] for row in eligible))
        return selected

    def test_manual_tests_before_and_between_eligible_tests_preserve_exact_filtered_union(self):
        for heavy_count in (2, 4):
            with self.subTest(heavy_count=heavy_count):
                self.names = ["ordinary", *(f"ActiveRegions/SimBudgetWorldParity.Replay/{n}"
                                          for n in range(heavy_count)), "last"]
                self.tests = [test(name) for name in self.names]
                manual = [test(f"manual-{n}", LABELS=["tools", "manual-gpu"]) for n in range(3)]
                roster = [manual[0], manual[1], *self.tests[:2], manual[2], *self.tests[2:]]
                selected = self.planned_selection(roster, self.tests)
                self.assertEqual([sum("SimBudgetWorldParity." in name for name in part)
                                  for part in selected], [heavy_count // 2] * 2)
                self.reports(selected)
                shards.merge(self.root, self.root / "merged.xml")
                cases = list(ET.parse(self.root / "merged.xml").getroot().iter("testcase"))
                self.assertEqual(sorted(case.get("name") for case in cases), sorted(self.names))

    def test_real_ctest_executes_exact_planned_index_file_selections(self):
        ctest = shutil.which("ctest")
        self.assertIsNotNone(ctest, "CTest is required for the shard index file integration check")
        for heavy_count in (2, 4):
            with self.subTest(heavy_count=heavy_count):
                project = self.root / f"ctest project {heavy_count}"
                project.mkdir()
                heavy = [f"ActiveRegions/SimBudgetWorldParity.Replay/{n}" for n in range(heavy_count)]
                literal = r"Suite.Literal[0](a+b)?^$|*\path{2} with space"
                names = ["manual-before", "ordinary", heavy[0], "manual-between",
                         *heavy[1:], "last", literal]
                commands = []
                for name in names:
                    commands.append(f"add_test([==[{name}]==] [==[{Path(sys.executable).as_posix()}]==] "
                                    "-I -B -c [==[print('executed')]==])")
                    if name.startswith("manual-"):
                        commands.append(f"set_tests_properties([==[{name}]==] "
                                        "PROPERTIES LABELS [==[tools;manual-gpu]==])")
                (project / "CTestTestfile.cmake").write_text("\n".join(commands) + "\n", encoding="utf-8")
                command = [ctest, "--test-dir", str(project)]
                for filename, arguments in (("ctest.all.json", []),
                                            ("ctest.inventory.json", ["-LE", "manual"])):
                    discovery = subprocess.run([*command, *arguments, "--show-only=json-v1"],
                                               capture_output=True, text=True, timeout=10)
                    self.assertEqual(discovery.returncode, 0, discovery.stdout + discovery.stderr)
                    (project / filename).write_text(discovery.stdout, encoding="utf-8")
                expected = [[*heavy[::2], literal], ["ordinary", *heavy[1::2], "last"]]
                for shard, planned in enumerate(expected, 1):
                    shards.plan(project, shard)
                    report = project / f"shard-{shard}.xml"
                    result = subprocess.run(
                        [*command, "-LE", "manual", "--no-tests=error", "-I",
                         str(project / "ctest.shard-index.txt"), "--parallel", "2",
                         "--output-junit", str(report)],
                        capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    cases = list(ET.parse(report).getroot().iter("testcase"))
                    self.assertEqual(sorted(case.get("name") for case in cases), sorted(planned))
                    self.assertTrue(all(case.get("status") == "run" for case in cases))
                    self.assertTrue(all(case.findtext("system-out").strip() == "executed" for case in cases))

    def test_ctest_integration_fails_clearly_when_ctest_is_missing(self):
        with patch.object(shutil, "which", return_value=None) as locate:
            with self.assertRaisesRegex(AssertionError, "CTest is required"):
                self.test_real_ctest_executes_exact_planned_index_file_selections()
        locate.assert_called_once_with("ctest")

    def test_mismatched_filtered_roster_or_order_refuses(self):
        manual = test("manual-first", LABELS=["manual"])
        (self.root / "ctest.all.json").write_text(json.dumps({"tests": [manual, *self.tests]}))
        for inventory in (self.tests[:-1], [*self.tests, manual], list(reversed(self.tests))):
            with self.subTest(inventory=inventory):
                (self.root / "ctest.inventory.json").write_text(json.dumps({"tests": inventory}))
                with self.assertRaisesRegex(ValueError, "filtered CTest roster or ordering"):
                    shards.plan(self.root, 1)

    def test_filtered_indices_select_literal_names_with_regex_metacharacters(self):
        literal = r"Suite.Literal[0](a+b)?^$|*\path{2} with space"
        self.names += [literal]
        self.tests = [test(name) for name in self.names]
        decoys = [test(name, LABELS=["manual"]) for name in
                  ("ordinary-extra", "prefix-ordinary", literal + "extra", "prefix" + literal,
                   "ActiveRegions/SimBudgetWorldParityXReplay/0")]
        self.planned_selection([*decoys, *self.tests], self.tests)

    def assert_execution_roster_drift_refuses(self, current):
        self.planned_selection(self.tests, self.tests)
        discovery = subprocess.CompletedProcess([], 0, stdout=json.dumps({"tests": current}))
        with patch.object(shards.subprocess, "run", return_value=discovery) as invoke:
            with self.assertRaisesRegex(ValueError, "execution CTest roster or ordering differs"):
                shards.run(self.root)
        # A drifted roster must never launch the actual tests, even if every
        # previously planned index is still valid (e.g. an appended test).
        invoke.assert_called_once()
        self.assertIn("--show-only=json-v1", invoke.call_args.args[0])

    def test_test_appended_after_discovery_refuses_execution(self):
        self.assert_execution_roster_drift_refuses([*self.tests, test("appended")])

    def test_test_removed_after_discovery_refuses_execution(self):
        self.assert_execution_roster_drift_refuses(self.tests[:-1])

    def test_roster_reordered_after_discovery_refuses_execution(self):
        self.assert_execution_roster_drift_refuses(list(reversed(self.tests)))

    def test_unchanged_roster_runs_in_the_same_ctest_context_and_preserves_status(self):
        self.planned_selection(self.tests, self.tests)
        for status in (0, 8):
            with self.subTest(status=status):
                discovery = subprocess.CompletedProcess([], 0, stdout=json.dumps({"tests": self.tests}))
                execution = subprocess.CompletedProcess([], status)
                with patch.object(shards.subprocess, "run", side_effect=[discovery, execution]) as invoke:
                    self.assertEqual(shards.run(self.root), status)
                check, execute = invoke.call_args_list
                common = ["ctest", "--preset", "debug-asan", "--test-dir", str(self.root), "-LE", "manual"]
                self.assertEqual(check.args[0], [*common, "--show-only=json-v1"])
                self.assertEqual(check.kwargs, {"check": True, "capture_output": True, "text": True})
                self.assertEqual(execute.args[0], [*common, "--no-tests=error", "-I",
                                                 str(self.root / "ctest.shard-index.txt"),
                                                 "--parallel", "2", "--timeout", "300",
                                                 "--output-junit", "ctest.junit.xml"])
                # Both calls inherit the wrapper's cwd and environment.
                self.assertEqual(execute.kwargs, {})

    def test_failed_or_malformed_execution_discovery_never_launches_tests(self):
        self.planned_selection(self.tests, self.tests)
        for response, error in ((subprocess.CalledProcessError(1, ["ctest"]), subprocess.CalledProcessError),
                                (subprocess.CompletedProcess([], 0, stdout="broken"), ValueError)):
            with self.subTest(error=error):
                with patch.object(shards.subprocess, "run", side_effect=[response]) as invoke:
                    with self.assertRaises(error):
                        shards.run(self.root)
                invoke.assert_called_once()

    def test_invalid_inventory_or_impossible_heavy_group_refuses(self):
        for tests, names in ((self.tests, []), (self.tests, ["unknown"]),
                             (self.tests, self.names + self.names[:1]),
                             (self.tests + self.tests[:1], self.names),
                             (self.tests[:2], self.names[:2])):
            with self.subTest(names=names), self.assertRaises(ValueError):
                shards.partition(tests, names)
        for row in self.tests[1:3]:
            row["properties"] = [{"name": "FIXTURES_REQUIRED", "value": ["shared"]}]
        with self.assertRaisesRegex(ValueError, "prevents balanced"):
            shards.partition(self.tests, self.names)

    def reports(self, parts=None):
        parts = parts if parts is not None else shards.partition(self.tests, self.names)
        for shard, names in enumerate(parts, 1):
            source = self.root / f"shard-{shard}"
            source.mkdir(exist_ok=True)
            (source / "ctest.inventory.json").write_text(json.dumps({"tests": self.tests}))
            root = ET.Element("testsuite")
            for name in names:
                case = ET.SubElement(root, "testcase", name=name)
                ET.SubElement(case, "system-out").text = f"receipt from {name}"
            ET.ElementTree(root).write(source / "ctest.junit.xml")

    def test_merge_preserves_output_and_complete_union(self):
        self.reports()
        output = self.root / "merged.xml"
        shards.merge(self.root, output)
        cases = list(ET.parse(output).getroot().iter("testcase"))
        self.assertEqual({case.get("name") for case in cases}, set(self.names))
        self.assertEqual(len(cases), len(self.names))
        self.assertTrue(all(case.findtext("system-out").startswith("receipt from ") for case in cases))

    def test_merge_accepts_explicit_run_status(self):
        self.reports()
        for shard in (1, 2):
            path = self.root / f"shard-{shard}/ctest.junit.xml"
            tree = ET.parse(path)
            for case in tree.getroot().iter("testcase"):
                case.set("status", "run")
            tree.write(path)
        shards.merge(self.root, self.root / "merged.xml")

    def test_merge_rejects_unevaluated_status_without_skipped_child(self):
        for shard in (1, 2):
            for status in ("disabled", "notrun", "", "unknown"):
                with self.subTest(shard=shard, status=status):
                    self.reports()
                    path = self.root / f"shard-{shard}/ctest.junit.xml"
                    tree = ET.parse(path)
                    tree.getroot()[0].set("status", status)
                    tree.write(path)
                    output = self.root / "merged.xml"
                    with self.assertRaisesRegex(ValueError, "unevaluated"):
                        shards.merge(self.root, output)
                    self.assertFalse(output.exists())

    def test_merge_rejects_executed_addition_or_removal_against_planned_roster(self):
        for change in ("append", "remove"):
            with self.subTest(change=change):
                self.reports()
                path = self.root / "shard-1/ctest.junit.xml"
                root = ET.parse(path).getroot()
                if change == "append":
                    ET.SubElement(root, "testcase", name="appended")
                else:
                    # Remove an ordinary case, preserving the heavy-test balance.
                    root.remove(next(case for case in root if case.get("name") == "ordinary"))
                ET.ElementTree(root).write(path)
                with self.assertRaisesRegex(ValueError, "executed shard union differs from the planned filtered"):
                    shards.merge(self.root, self.root / "merged.xml")
                self.assertFalse((self.root / "merged.xml").exists())

    def test_merged_report_passes_the_unchanged_floor_and_required_probe_checks(self):
        arguments = aggregate_inventory_arguments()
        self.names += [arguments[index + 1] for index, argument in enumerate(arguments)
                       if argument == "--require-test"]
        self.names += ["AmbientDeterminismReceipt.EmitsConstructorAndEvolvedHashes",
                       "AmbientInputBounds.UnpaddedTwoElementInputFaultsAtActiveIsa"]
        self.names += [f"ordinary-{index}" for index in range(2320 - len(self.names))]
        self.tests = [test(name) for name in self.names]
        self.reports()
        for shard in (1, 2):
            path = self.root / f"shard-{shard}/ctest.junit.xml"
            tree = ET.parse(path)
            for case in tree.getroot().iter("testcase"):
                if case.get("name").startswith("AmbientDeterminismReceipt."):
                    case.find("system-out").text = (
                        "AMBIENT_RECEIPT version=1 seed=424242 ticks=64 ceiling=256 "
                        "cpu_simd=256 active_simd=256 wind_ctor=aa11 wind_evolved=bb22 "
                        "weather_ctor=cc33 weather_evolved=dd44 aether_ctor=ee55 aether_evolved=ff66")
                if case.get("name").startswith("AmbientInputBounds."):
                    case.find("system-out").text = "AMBIENT_PROBE probe_status=ran active_simd=256"
            tree.write(path)
        output = self.root / "merged.xml"
        shards.merge(self.root, output)
        for script, arguments in (
            ("check_ctest_junit.py", [str(output), *arguments]),
            ("compare_ambient_receipts.py", [f"asan={output}", "--expect-labels", "asan", "--require-probe", "asan"]),
        ):
            result = subprocess.run([sys.executable, "-I", "-B", str(Path(__file__).with_name(script)), *arguments],
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_complete_shard_union_cannot_replace_a_required_test_with_extra_coverage(self):
        arguments = aggregate_inventory_arguments()
        required = [arguments[index + 1] for index, argument in enumerate(arguments)
                    if argument == "--require-test"]
        self.assertGreaterEqual(len(required), 3)
        self.names += required
        self.names += [f"ordinary-{index}" for index in range(2320 - len(self.names))]
        complete = self.names[:]
        for name in required:
            with self.subTest(missing=name):
                self.names = [item if item != name else "replacement-coverage" for item in complete]
                self.tests = [test(item) for item in self.names]
                self.reports()
                output = self.root / "merged.xml"
                shards.merge(self.root, output)
                result = subprocess.run(
                    [sys.executable, "-I", "-B", str(Path(__file__).with_name("check_ctest_junit.py")),
                     str(output), *arguments], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn(f"Required CTest cases are missing:\n  - {name}\n", result.stderr)

    def test_complete_shard_union_below_inventory_floor_is_rejected(self):
        self.names += [f"ordinary-{index}" for index in range(2319 - len(self.names))]
        self.tests = [test(name) for name in self.names]
        self.reports()
        output = self.root / "merged.xml"
        shards.merge(self.root, output)
        result = subprocess.run(
            [sys.executable, "-I", "-B", str(Path(__file__).with_name("check_ctest_junit.py")),
             str(output), "--minimum-tests", "2320"],
            capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("evaluated 2319 tests, requires at least 2320", result.stderr)

    def test_empty_missing_duplicate_short_extra_and_unbalanced_shards_refuse(self):
        for parts in (([], [1, 2, 3, 4, 5, 6]), ([1, 2, 4, 5], [3]),
                      ([1, 2, 4, 5], [3, 3, 6]), ([1, 2, 4, 5], [1, 3, 6]),
                      ([1, 2, 3, 4], [5, 6])):
            with self.subTest(parts=parts), self.assertRaises(ValueError):
                self.reports([[self.names[index - 1] for index in part] for part in parts])
                shards.merge(self.root, self.root / "merged.xml")
        self.reports()
        path = self.root / "shard-2/ctest.junit.xml"
        root = ET.parse(path).getroot()
        root[0].set("name", "unexpected")
        ET.ElementTree(root).write(path)
        with self.assertRaisesRegex(ValueError, "union differs"):
            shards.merge(self.root, self.root / "merged.xml")
        path.unlink()
        with self.assertRaises(OSError):
            shards.merge(self.root, self.root / "merged.xml")

    def test_failed_skipped_malformed_and_mismatched_reports_refuse(self):
        for tag in ("failure", "error", "skipped"):
            self.reports()
            path = self.root / "shard-1/ctest.junit.xml"
            root = ET.parse(path).getroot()
            ET.SubElement(root[0], tag)
            ET.ElementTree(root).write(path)
            with self.subTest(tag=tag), self.assertRaisesRegex(ValueError, "unevaluated"):
                shards.merge(self.root, self.root / "merged.xml")
        self.reports()
        (self.root / "shard-2/ctest.inventory.json").write_text(json.dumps({"tests": self.tests[:-1]}))
        with self.assertRaisesRegex(ValueError, "different CTest inventories"):
            shards.merge(self.root, self.root / "merged.xml")
        self.reports()
        (self.root / "shard-2/ctest.junit.xml").write_text("broken")
        with self.assertRaises(ET.ParseError):
            shards.merge(self.root, self.root / "merged.xml")


if __name__ == "__main__":
    unittest.main()
