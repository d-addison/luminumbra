#!/usr/bin/env python3
"""Validate software-render evidence without starting a renderer or display."""
import importlib.util
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET

SPEC = importlib.util.spec_from_file_location("software_gl", Path(__file__).with_name("run_software_gl.py"))
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class SoftwareGlEvidenceTests(unittest.TestCase):
    def report(self):
        root = ET.Element("testsuites")
        suite = ET.SubElement(root, "testsuite")
        for index in range(15):
            ET.SubElement(suite, "testcase", classname="Rendering", name=f"Case{index}", status="run")
        return root

    def test_successful_google_test_forms(self):
        root = self.report()
        MODULE.validate_report(root)
        for case in root.iter("testcase"):
            case.set("result", "completed")
        MODULE.validate_report(root)

    def test_unevaluated_and_failed_cases_are_rejected(self):
        for field, value in (("status", "disabled"), ("status", "notrun"),
                             ("status", "unknown"), ("status", ""),
                             ("result", "disabled"), ("result", "notrun"),
                             ("result", "skipped"), ("result", "suppressed"),
                             ("result", "unknown"), ("result", "")):
            root = self.report()
            next(root.iter("testcase")).set(field, value)
            with self.subTest(field=field, value=value), self.assertRaises(RuntimeError):
                MODULE.validate_report(root)
        for tag in ("failure", "error", "skipped"):
            root = self.report()
            ET.SubElement(next(root.iter("testcase")), tag)
            with self.subTest(tag=tag), self.assertRaises(RuntimeError):
                MODULE.validate_report(root)

    def test_empty_partial_and_duplicate_inventories_are_rejected(self):
        partial = self.report()
        suite = partial.find("testsuite")
        suite.remove(suite[0])
        for root in (ET.Element("testsuites"), partial):
            with self.assertRaises(RuntimeError):
                MODULE.validate_report(root)
        duplicate = self.report()
        suite = duplicate.find("testsuite")
        suite[-1].set("name", suite[0].get("name"))
        with self.assertRaises(RuntimeError):
            MODULE.validate_report(duplicate)


if __name__ == "__main__":
    unittest.main()
