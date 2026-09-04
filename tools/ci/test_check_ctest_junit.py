#!/usr/bin/env python3

import importlib.util
import re
import sys
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path


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
    def setUp(self):
        self.context_pattern = re.compile(
            r"(?:no GL context: )?glfwCreateWindow failed"
            r"(?: \(no GL 4\.5 context(?: available)?\))?"
        )

    def test_rejects_audio_skip_now_that_a_fixture_exists(self):
        # This skip used to be allow-listed because the sound banks are external
        # to the repository. AudioBankIntegrity now generates its payloads at test
        # time, so it runs for real and a skip from it is a genuine failure.
        case = skipped_case(
            "AudioBankIntegrity.LoadedBankFilesExistOnDisk",
            "audio binaries are intentionally external to the repository",
        )
        self.assertFalse(MODULE.skip_is_allowed(case, []))

    def test_allow_list_is_empty(self):
        # Every previously allow-listed test now genuinely runs. Guard against a
        # skip quietly being re-admitted here without justification.
        self.assertEqual(MODULE.ALLOWED_SKIPS, set())

    def test_accepts_observed_explicit_context_reasons(self):
        reasons = (
            "glfwCreateWindow failed",
            "no GL context: glfwCreateWindow failed",
            "glfwCreateWindow failed (no GL 4.5 context)",
            "glfwCreateWindow failed (no GL 4.5 context available)",
            "no GL context: glfwCreateWindow failed (no GL 4.5 context available)",
        )
        for reason in reasons:
            with self.subTest(reason=reason):
                case = skipped_case("RenderSmokeTest.Draws", reason)
                self.assertTrue(MODULE.skip_is_allowed(case, [self.context_pattern]))
                self.assertFalse(MODULE.skip_is_allowed(case, []))

    def test_rejects_unrelated_or_missing_reason(self):
        unrelated = skipped_case("RenderSmokeTest.Draws", "shader compile failed")
        missing = skipped_case("RenderSmokeTest.Draws", None)
        self.assertFalse(MODULE.skip_is_allowed(unrelated, [self.context_pattern]))
        self.assertFalse(MODULE.skip_is_allowed(missing, [self.context_pattern]))


if __name__ == "__main__":
    unittest.main()
