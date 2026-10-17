#!/usr/bin/env python3

import contextlib
import importlib.util
import io
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest import mock


MODULE_PATH = Path(__file__).with_name("check_ctest_junit.py")
SPEC = importlib.util.spec_from_file_location("check_ctest_junit", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def skipped_case(name: str, reason: str | None) -> ET.Element:
    case = ET.Element("testcase", {"name": name})
    ET.SubElement(case, "skipped")
    output = ET.SubElement(case, "system-out")
    if reason is not None:
        output.text = f"source.cpp(1): Skipped\n{reason}\n"
    return case


class CTestSkipAuditTests(unittest.TestCase):
    def test_non_applicable_probe_passes_without_a_skip(self):
        status = "not_applicable:guard-page probe is Linux only"
        root = ET.Element("testsuite")
        case = ET.SubElement(
            root,
            "testcase",
            name="AmbientInputBounds.UnpaddedTwoElementInputFaultsAtActiveIsa",
            probe_status=status,
        )
        ET.SubElement(case, "system-out").text = f"AMBIENT_PROBE probe_status={status}\n"
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "ctest.junit.xml"
            ET.ElementTree(root).write(report, encoding="utf-8")
            output = io.StringIO()
            with mock.patch.object(sys, "argv", [str(MODULE_PATH), str(report)]):
                with contextlib.redirect_stdout(output):
                    self.assertEqual(MODULE.main(), 0)
            self.assertIn("1 tests; 0 approved skips", output.getvalue())

    def test_rejects_audio_skip_now_that_a_fixture_exists(self):
        # This skip used to be allow-listed because the sound banks are external
        # to the repository. AudioBankIntegrity now generates its payloads at test
        # time, so it runs for real and a skip from it is a genuine failure.
        case = skipped_case(
            "AudioBankIntegrity.LoadedBankFilesExistOnDisk",
            "audio binaries are intentionally external to the repository",
        )
        self.assertFalse(MODULE.skip_is_allowed(case))

    def audit_required_tests(self, names, required, expected, skipped=False, status=None):
        root = ET.Element("testsuite")
        for name in names:
            case = ET.SubElement(root, "testcase")
            if name is not None:
                case.set("name", name)
            if status is not None:
                case.set("status", status)
            if skipped:
                ET.SubElement(case, "skipped")
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "ctest.junit.xml"
            ET.ElementTree(root).write(report, encoding="utf-8")
            arguments = [str(MODULE_PATH), str(report), "--minimum-tests", str(len(names))]
            for name in required:
                arguments.extend(("--require-test", name))
            output, errors = io.StringIO(), io.StringIO()
            with mock.patch.object(sys, "argv", arguments):
                with contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
                    self.assertEqual(MODULE.main(), expected)
            return output.getvalue(), errors.getvalue()

    def test_repeated_required_names_pass_with_additional_coverage(self):
        output, errors = self.audit_required_tests(
            ["Developer.First", "Developer.Second", "Unrelated.Extra"],
            ["Developer.First", "Developer.Second"], 0)
        self.assertIn("3 tests; 0 approved skips", output)
        self.assertEqual(errors, "")

    def test_missing_required_names_fail_even_when_the_count_floor_passes(self):
        required = ["Developer.First", "Developer.Second"]
        for names in (["Developer.First", "Unrelated.Extra"],
                      ["Developer.Second", "Unrelated.Extra"],
                      ["Unrelated.First", "Unrelated.Second"],
                      ["Developer.FirstSuffix", "PrefixDeveloper.Second"],
                      [None, None], []):
            with self.subTest(names=names):
                output, errors = self.audit_required_tests(names, required, 1)
                self.assertEqual(output, "")
                missing = sorted(set(required) - set(names))
                self.assertEqual(errors, "Required CTest cases are missing:\n" +
                                 "".join(f"  - {name}\n" for name in missing))

    def test_required_name_does_not_allow_a_skipped_case(self):
        output, errors = self.audit_required_tests(
            ["Developer.First"], ["Developer.First"], 1, skipped=True)
        self.assertEqual(output, "")
        self.assertIn("Unexpected unevaluated tests:\n  - Developer.First\n", errors)

    def test_explicit_run_status_passes_inventory_and_required_name_checks(self):
        output, errors = self.audit_required_tests(
            ["Developer.First"], ["Developer.First"], 0, status="run")
        self.assertIn("1 tests; 0 approved skips", output)
        self.assertEqual(errors, "")

    def test_unevaluated_status_cannot_satisfy_inventory_or_required_name_checks(self):
        for status in ("disabled", "notrun", "", "unknown"):
            with self.subTest(status=status):
                output, errors = self.audit_required_tests(
                    ["Developer.First"], ["Developer.First"], 1, status=status)
                self.assertEqual(output, "")
                self.assertIn("Unexpected unevaluated tests:\n  - Developer.First\n", errors)

    def test_required_target_passes_when_registered(self):
        self.check_required_test("DeveloperConsoleUi.ScopedContextRestoresPreviousContext", 0)

    def test_missing_target_fails_even_when_inventory_floor_is_met(self):
        self.check_required_test("UiFixture.MissingTarget", 1)

    def check_required_test(self, required: str, expected: int):
        output, errors = self.audit_required_tests(
            ["DeveloperConsoleUi.ScopedContextRestoresPreviousContext", "Unrelated.ExtraCoverage"],
            [required], expected)
        if expected:
            self.assertIn("Required CTest cases are missing:", errors)
            self.assertIn(required, errors)
        else:
            self.assertIn("2 tests; 0 approved skips", output)

    def test_allow_list_is_empty(self):
        # Every previously allow-listed test now genuinely runs. Guard against a
        # skip quietly being re-admitted here without justification.
        self.assertEqual(MODULE.ALLOWED_SKIPS, set())

    def test_rejects_skip_regardless_of_reason(self):
        explicit = skipped_case("RenderSmokeTest.Draws", "glfwCreateWindow failed")
        missing = skipped_case("RenderSmokeTest.Draws", None)
        self.assertFalse(MODULE.skip_is_allowed(explicit))
        self.assertFalse(MODULE.skip_is_allowed(missing))


if __name__ == "__main__":
    unittest.main()
