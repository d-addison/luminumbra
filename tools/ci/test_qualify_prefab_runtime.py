"""Refuse incomplete or unevaluated prefab results in the existing CTest report."""

import unittest
import xml.etree.ElementTree as ET

from qualify_prefab_runtime import (
    REQUIRED_CASES, REQUIRED_INSTALLED_CHECKS, validate_acceptance, validate_ctest,
)


class PrefabCtestGateTest(unittest.TestCase):
    def report(self):
        root = ET.Element("testsuite")
        for name in sorted(REQUIRED_CASES):
            ET.SubElement(root, "testcase", name=name, status="run")
        # The ordinary engine suite shares the report and is audited separately.
        ET.SubElement(root, "testcase", name="Unrelated.Test", status="run")
        return root

    def test_accepts_complete_component_in_larger_report(self):
        self.assertEqual(validate_ctest(self.report()), sorted(REQUIRED_CASES))

    def test_refuses_empty_missing_and_duplicate_cases(self):
        root = self.report()
        root.remove(root[0])
        duplicate = self.report()
        duplicate.append(duplicate[0])
        for report in (ET.Element("testsuite"), root, duplicate):
            with self.subTest(report=ET.tostring(report)), self.assertRaises(ValueError):
                validate_ctest(report)

    def test_refuses_failures_errors_skips_and_nonrun_status(self):
        for tag in ("failure", "error", "skipped"):
            root = self.report()
            ET.SubElement(root[0], tag)
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                validate_ctest(root)
        for status in ("notrun", "disabled", ""):
            root = self.report()
            root[0].set("status", status)
            with self.subTest(status=status), self.assertRaises(ValueError):
                validate_ctest(root)

    def test_new_component_tests_must_also_run_and_pass(self):
        root = self.report()
        extra = ET.SubElement(root, "testcase", name="PrefabRuntimeTest.FutureCase", status="run")
        self.assertEqual(len(validate_ctest(root)), len(REQUIRED_CASES) + 1)
        ET.SubElement(extra, "skipped")
        with self.assertRaises(ValueError):
            validate_ctest(root)


    def test_refuses_suppressed_result_even_with_run_status(self):
        root = self.report()
        root[0].set("result", "suppressed")
        with self.assertRaises(ValueError):
            validate_ctest(root)


class InstalledAcceptanceGateTest(unittest.TestCase):
    def receipt(self):
        return {"passed": True, "checks": [
            {"name": name, "passed": True} for name in sorted(REQUIRED_INSTALLED_CHECKS)]}

    def test_accepts_complete_named_obligations(self):
        self.assertEqual(validate_acceptance(self.receipt()), 19)

    def test_refuses_missing_obligation_despite_same_count(self):
        receipt = self.receipt()
        receipt["checks"][0]["name"] = "Unrelated check"
        with self.assertRaises(ValueError):
            validate_acceptance(receipt)

    def test_refuses_duplicates_failed_checks_and_incomplete_receipt(self):
        duplicate = self.receipt()
        duplicate["checks"].append(duplicate["checks"][0])
        failed = self.receipt()
        failed["checks"][0]["passed"] = False
        incomplete = self.receipt()
        incomplete["passed"] = False
        for receipt in (duplicate, failed, incomplete, {}):
            with self.subTest(receipt=receipt), self.assertRaises(ValueError):
                validate_acceptance(receipt)


if __name__ == "__main__":
    unittest.main()
