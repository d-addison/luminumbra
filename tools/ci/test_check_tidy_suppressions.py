#!/usr/bin/env python3
"""Deterministic tests and the live repository suppression-budget gate."""

import copy
from contextlib import redirect_stdout
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("tidy_suppressions", Path(__file__).with_name(
    "check_tidy_suppressions.py"))
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
GLOBAL = "cppcoreguidelines-avoid-non-const-global-variables"
COMPLEXITY = "readability-function-cognitive-complexity"
CHECKS = [GLOBAL, COMPLEXITY, "clang-analyzer-core*"]


def scan(text, path="src/example.cpp"):
    return MODULE.suppressions(path, text, CHECKS)


def inventory(text="", exemptions=()):
    return {"version": 1, "gate_checks": CHECKS, "suppressions": scan(text),
            "tracked_global_tus": list(exemptions)}


class ScannerTests(unittest.TestCase):
    def test_inline_nextline_and_block_annotations(self):
        source = (f"namespace State {{\n// NOLINTNEXTLINE({GLOBAL})\nint counter;\n"
                  f"int other; // NOLINT({GLOBAL})\n"
                  f"// NOLINTBEGIN({GLOBAL})\nint third;\n// NOLINTEND({GLOBAL})\n}}\n")
        found = scan(source)
        self.assertEqual([item["directive"] for item in found],
                         ["NOLINTNEXTLINE", "NOLINT", "NOLINTBEGIN"])
        self.assertEqual([item["target"] for item in found],
                         ["int counter ;", "int other ;", "int third ;"])
        self.assertTrue(all(item["scope"] == ["namespace State"] for item in found))

    def test_clang_recognizes_directives_inside_literals(self):
        for literal in ('"NOLINT"', 'R"tag(NOLINTNEXTLINE(*)\n)tag"',
                        'u8R"(NOLINT(*))"', 'LR"x(NOLINT)x"', '"\\\"NOLINT"'):
            with self.subTest(literal=literal):
                self.assertEqual(len(scan("auto text = " + literal + ";")), 1)
        self.assertEqual(scan("char c = 'N';"), [])

    def test_unrelated_checks_empty_lists_and_negative_globs_do_not_count(self):
        self.assertEqual(scan("int x; // NOLINT(readability-identifier-naming)\n"
                              "int y; // NOLINT()\nint z; // NOLINT(-*)\n"
                              "int NOLINTED; // NOLINTED\n"), [])

    def test_annotations_in_preprocessor_comments_are_counted(self):
        self.assertEqual(len(scan(f"#define VALUE 1 // NOLINT({GLOBAL})\n")), 1)
        source = f'#include "NOLINT"\nnamespace State {{\nint x; // NOLINT({GLOBAL})\n}}'
        self.assertEqual(len(scan(source)), 2)
        self.assertEqual(scan(source)[1]["scope"], ["namespace State"])

    def test_clang_accepted_word_prefixes_and_spaced_blanket_suppressions(self):
        self.assertEqual(scan(f"int x; // XNOLINT({GLOBAL})")[0]["checks"], [GLOBAL])
        self.assertEqual(scan("int x; // NOLINT (unrelated-check)")[0]["checks"], sorted(CHECKS))

    def test_bare_wildcard_and_multiline_comments(self):
        for annotation, affected in (("NOLINT", sorted(CHECKS)),
                                     ("NOLINTNEXTLINE(*)", sorted(CHECKS)),
                                     ("NOLINTNEXTLINE(cppcoreguidelines-*)", [GLOBAL]),
                                     ("NOLINTNEXTLINE(clang-*-core.NullDereference)",
                                      ["clang-analyzer-core*"])):
            with self.subTest(annotation=annotation):
                found = scan("/* explanation\n" + annotation + " */\nint x;\n")
                self.assertEqual(len(found), 1)
                self.assertEqual(found[0]["checks"], affected)

    def test_formatting_and_line_numbers_are_not_identities(self):
        first = scan(f"namespace State {{\n// NOLINTNEXTLINE({GLOBAL})\nint counter = 1;\n}}")
        second = scan(f"\n\nnamespace   State\n{{\n// NOLINTNEXTLINE({GLOBAL})\n"
                      "int\n counter=\n1;\n}")
        self.assertEqual(first, second)

    def test_scope_and_construct_distinguish_allowances(self):
        source = f"void First() {{\nint x; // NOLINT({GLOBAL})\n}}\n"
        self.assertNotEqual(scan(source), scan(source.replace("First", "Second")))
        self.assertNotEqual(scan(source), scan(source.replace("int x", "int y")))
        self.assertNotEqual(scan(source), scan(source, "test/example.cpp"))

    def test_empty_and_annotation_at_end_of_file(self):
        self.assertEqual(scan(""), [])
        self.assertEqual(scan("// NOLINTNEXTLINE(*)")[0]["target"], "")

    def test_check_glob_intersections(self):
        for first, second, expected in (("a*bc", "ab*c", True), ("a*", "b*", False),
                                        ("*", "", True), ("", "x", False),
                                        ("ab", "abc", False), ("abc", "abc", True)):
            with self.subTest(first=first, second=second):
                self.assertEqual(MODULE.globs_overlap(first, second), expected)
                self.assertEqual(MODULE.globs_overlap(second, first), expected)


class BudgetTests(unittest.TestCase):
    def test_new_suppression_is_refused(self):
        self.assertTrue(MODULE.budget_errors(inventory(f"int x; // NOLINT({GLOBAL})"), inventory()))

    def test_removing_one_does_not_fund_another_symbol_or_file(self):
        baseline = inventory(f"int x; // NOLINT({GLOBAL})")
        for changed in (inventory(f"int y; // NOLINT({GLOBAL})"), copy.deepcopy(baseline)):
            if changed == baseline:
                changed["suppressions"][0]["path"] = "test/moved.cpp"
            self.assertTrue(MODULE.budget_errors(changed, baseline))

    def test_copying_identical_annotation_grows_budget(self):
        source = f"int x; // NOLINT({GLOBAL})\n"
        self.assertTrue(MODULE.budget_errors(inventory(source * 2), inventory(source)))

    def test_broadening_a_suppression_within_one_check_family_is_refused(self):
        baseline = inventory("int x; // NOLINT(clang-analyzer-core.NullDereference)")
        broader = inventory("int x; // NOLINT(clang-analyzer-core*)")
        self.assertTrue(MODULE.budget_errors(broader, baseline))

    def test_new_whole_tu_exemption_is_refused(self):
        baseline = inventory(exemptions=("src/old.cpp",))
        for exemptions in (("src/new.cpp",), ("src/old.cpp", "src/new.cpp")):
            self.assertTrue(MODULE.budget_errors(inventory(exemptions=exemptions), baseline))

    def test_retired_entries_must_be_removed_and_shrinking_passes(self):
        previous = inventory(f"int x; // NOLINT({GLOBAL})", ("src/old.cpp",))
        smaller = inventory()
        self.assertTrue(MODULE.budget_errors(smaller, previous))
        self.assertEqual(MODULE.budget_errors(smaller, smaller), [])
        self.assertEqual(MODULE.budget_errors(previous, previous), [])

    def test_malformed_baseline_and_check_set_changes_fail_closed(self):
        actual = inventory()
        for baseline in (None, [], {}, {**actual, "version": 2},
                         {**actual, "gate_checks": []}, {**actual, "suppressions": None},
                         {**actual, "tracked_global_tus": "src/unit.cpp"}):
            with self.subTest(baseline=baseline):
                self.assertTrue(MODULE.budget_errors(actual, baseline))


class PolicyTests(unittest.TestCase):
    def test_policy_reads_all_fragments_comments_and_empty_exemptions(self):
        script = ('gate_checks="-*"\ngate_checks+=",modernize-use-nullptr"\n'
                  'gate_checks+=",clang-analyzer-core*"\ntracked_global_tus=(\n'
                  ' # explanation\n "src/space name.cpp" # reason\n)\n')
        self.assertEqual(MODULE.gate_policy(script),
                         (["clang-analyzer-core*", "modernize-use-nullptr"], ["src/space name.cpp"]))
        self.assertEqual(MODULE.gate_policy(script.replace(' "src/space name.cpp" # reason\n', ""))[1], [])

    def test_malformed_policy_and_exemption_paths_fail_closed(self):
        for script in ("", 'gate_checks="modernize-use-nullptr"\ntracked_global_tus=(\n)\n',
                       'gate_checks="-*,"\ntracked_global_tus=(\n)\n'):
            with self.subTest(script=script), self.assertRaises(ValueError):
                MODULE.gate_policy(script)
        for path in ("vendor/a.cpp", "src/../other.cpp", "/src/a.cpp", "src/a.haha", "src/header.h", "$entry"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                MODULE.gate_policy('gate_checks="-*,modernize-use-nullptr"\n'
                                   'tracked_global_tus=(\n"' + path + '"\n)\n')
        with self.assertRaises(ValueError):
            MODULE.gate_policy('gate_checks="-*,modernize-use-nullptr"\n'
                               'tracked_global_tus=(\n"src/a.cpp" "src/a.cpp"\n)\n')

    def test_regular_descriptor_crlf_missing_and_special_file(self):
        with tempfile.TemporaryDirectory(prefix="tidy-budget-") as directory:
            path = Path(directory) / "source.cpp"
            path.write_bytes(b"int x;\r\n")
            self.assertEqual(MODULE.read_text(path), "int x;\n")
            with patch.object(MODULE.os, "fstat", return_value=os.stat_result(
                    (stat.S_IFIFO, 0, 0, 0, 0, 0, 0, 0, 0, 0))), self.assertRaises(ValueError):
                MODULE.read_text(path)
            with self.assertRaises(OSError):
                MODULE.read_text(Path(directory) / "absent.cpp")

    def test_discovery_includes_untracked_sources_and_retires_deleted_source(self):
        with tempfile.TemporaryDirectory(prefix="tidy-budget-") as directory:
            root = Path(directory)
            (root / "scripts").mkdir()
            (root / "scripts/tidy.sh").write_text(
                'gate_checks="-*,modernize-use-nullptr"\ntracked_global_tus=(\n)\n', encoding="utf-8")
            (root / "tools").mkdir()
            (root / "tools/new.cpp").write_text('int* x = 0; // NOLINT(modernize-use-nullptr)\n',
                                               encoding="utf-8")
            with patch.object(MODULE.subprocess, "check_output", return_value=b"tools/new.cpp\0"):
                self.assertEqual(len(MODULE.inventory(root)["suppressions"]), 1)
                (root / "tools/new.cpp").unlink()
                self.assertEqual(MODULE.inventory(root)["suppressions"], [])

    def test_unreadable_and_partial_inputs_fail_closed(self):
        for error in (PermissionError("unreadable"), UnicodeError("partial text")):
            with self.subTest(error=error), patch.object(MODULE, "read_text", side_effect=error), \
                    self.assertRaises(type(error)):
                MODULE.inventory(ROOT)

    def test_cli_refuses_missing_policy(self):
        with tempfile.TemporaryDirectory(prefix="tidy-budget-") as directory:
            result = subprocess.run([sys.executable, "-I", "-B", str(ROOT / "tools/ci/check_tidy_suppressions.py"),
                                     "--root", directory], capture_output=True, text=True, check=False)
            self.assertEqual(result.returncode, 1)
            self.assertIn("tidy suppression budget:", result.stderr)

    def test_repository_budget(self):
        self.assertEqual(MODULE.budget_errors(MODULE.inventory(ROOT),
                         json.loads(MODULE.read_text(ROOT / MODULE.BASELINE))), [])


class HeaderSmokeTests(unittest.TestCase):
    def test_exact_root_filter_covers_both_separators_and_excludes_dependencies(self):
        root = ROOT / "path with spaces (and)+metacharacters"
        pattern = re.compile(MODULE.absolute_header_filter(root))
        for source in ("src", "include", "test", "tools"):
            path = root.resolve().as_posix() + "/" + source + "/probe.h"
            self.assertIsNotNone(pattern.search(path))
            self.assertIsNotNone(pattern.search(path.replace("/", "\\")))
        for path in ("vendor/include/probe.h", "_deps/dependency/src/probe.h",
                     "build/include/generated.h", "other/tools/probe.h", "src-neighbor/probe.h"):
            self.assertIsNone(pattern.search(root.resolve().as_posix() + "/" + path))
        self.assertIsNone(pattern.search(root.resolve().as_posix() + "-neighbor/src/probe.h"))

    def smoke_fixture(self, directory, command):
        root = Path(directory)
        (root / "scripts").mkdir()
        (root / "scripts/tidy.sh").write_text(MODULE.read_text(ROOT / "scripts/tidy.sh"), encoding="utf-8")
        database = root / "compile_commands.json"
        database.write_text(json.dumps(command), encoding="utf-8")
        return root, database

    def test_smoke_borrows_compile_command_and_measures_both_filters(self):
        for command in ({"arguments": ["clang++", "-c", "original.cpp"]},
                        {"command": "clang++ -c original.cpp"}):
            with self.subTest(command=command), tempfile.TemporaryDirectory(prefix="tidy-budget-") as directory:
                root, database = self.smoke_fixture(directory, [
                    {"directory": directory, "file": "original.cpp", **command}])
                current = subprocess.CompletedProcess([], 0, "", "1 warning suppressed\n")
                candidate = subprocess.CompletedProcess([], 1,
                    "use nullptr [modernize-use-nullptr,-warnings-as-errors]\n", "")
                with patch.object(MODULE.subprocess, "run", side_effect=[current, candidate]) as run, \
                        redirect_stdout(io.StringIO()) as output:
                    MODULE.header_smoke(root, database)
                self.assertEqual(run.call_count, 2)
                self.assertIn("current: exit=0; modernize-use-nullptr=0", output.getvalue())
                self.assertIn("candidate: exit=1; modernize-use-nullptr=1", output.getvalue())
                config = json.loads(next(arg[len("--config="):] for arg in run.call_args.args[0]
                                         if arg.startswith("--config=")))
                self.assertIsNotNone(re.search(config["HeaderFilterRegex"],
                                              root.as_posix() + "/test/fixtures/tidy_header_smoke.h"))

    def test_smoke_refuses_invalid_databases_and_missing_gate_configuration(self):
        for commands in ([], {}, [None], [{"directory": "x"}],
                         [{"directory": "x", "file": "x", "arguments": [7]}]):
            with self.subTest(commands=commands), tempfile.TemporaryDirectory(prefix="tidy-budget-") as directory:
                root, database = self.smoke_fixture(directory, commands)
                with self.assertRaises(ValueError):
                    MODULE.header_smoke(root, database)
        for old, new in (("gate_config='", "other_config='"),
                         ('"HeaderFilterRegex": "^(src|include|test)/"', '"DifferentOption": ""')):
            with self.subTest(old=old), tempfile.TemporaryDirectory(prefix="tidy-budget-") as directory:
                root, database = self.smoke_fixture(directory, [
                    {"directory": directory, "file": "x", "arguments": ["clang++", "x"]}])
                script = root / "scripts/tidy.sh"
                script.write_text(MODULE.read_text(script).replace(old, new), encoding="utf-8")
                with self.assertRaises(ValueError):
                    MODULE.header_smoke(root, database)

    def test_smoke_refuses_compiler_errors_missing_or_duplicate_diagnostics(self):
        for code, output in ((1, "compile error [clang-diagnostic-error]"), (0, ""),
                             (1, "use nullptr [modernize-use-nullptr,-warnings-as-errors]\n" * 2),
                             (2, "crashed")):
            with self.subTest(code=code, output=output), \
                    tempfile.TemporaryDirectory(prefix="tidy-budget-") as directory:
                root, database = self.smoke_fixture(directory, [
                    {"directory": directory, "file": "x", "arguments": ["clang++", "x"]}])
                with patch.object(MODULE.subprocess, "run", side_effect=[
                        subprocess.CompletedProcess([], 0, "", ""),
                        subprocess.CompletedProcess([], code, output, "")]), \
                        redirect_stdout(io.StringIO()), self.assertRaises(ValueError):
                    MODULE.header_smoke(root, database)


if __name__ == "__main__":
    unittest.main()
