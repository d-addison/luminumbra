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
