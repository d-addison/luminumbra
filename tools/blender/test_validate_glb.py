#!/usr/bin/env python3
"""Tests for the standalone luminumbra GLB validator."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


HERE = Path(__file__).resolve().parent
VALIDATOR = HERE / "validate_glb.py"
FIXTURE_GENERATOR = HERE / "fixtures" / "make_fixtures.py"


class ValidateGlbTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary_directory = tempfile.TemporaryDirectory(prefix="luminumbra_glb_")
        cls.fixture_dir = Path(cls.temporary_directory.name)
        result = subprocess.run(
            [sys.executable, str(FIXTURE_GENERATOR), "--output-dir", str(cls.fixture_dir)],
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise RuntimeError(f"fixture generation failed: {result.stderr}")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temporary_directory.cleanup()

    def validate(self, fixture: str) -> tuple[subprocess.CompletedProcess[str], dict]:
        result = subprocess.run(
            [sys.executable, str(VALIDATOR), str(self.fixture_dir / fixture)],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertTrue(result.stdout, result.stderr)
        return result, json.loads(result.stdout)

    def assert_report_shape(self, report: dict) -> None:
        self.assertIn("valid", report)
        self.assertIn("profile", report)
        self.assertIsInstance(report.get("findings"), list)
        for finding in report["findings"]:
            self.assertIsInstance(finding.get("rule_id"), str)
            self.assertEqual(finding.get("severity"), "error")
            location = finding.get("location")
            self.assertIsInstance(location, dict)
            self.assertEqual(
                set(location),
                {"mesh", "primitive", "joint", "accessor", "vertex", "attribute", "extension"},
            )

    def test_clean_static(self) -> None:
        result, report = self.validate("clean_static.glb")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertTrue(report["valid"])
        self.assertEqual(report["profile"], "static")
        self.assertEqual(report["findings"], [])
        self.assert_report_shape(report)

    def test_clean_skinned(self) -> None:
        result, report = self.validate("clean_skinned.glb")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertTrue(report["valid"])
        self.assertEqual(report["profile"], "skinned")
        self.assertEqual(report["findings"], [])
        self.assert_report_shape(report)

    def test_seeded_defects(self) -> None:
        expected = {
            "missing_normal.glb": "attribute.normal",
            "nonindexed.glb": "primitive.indexed",
            "unsupported_extension.glb": "extension.unsupported",
            "mixed_geometry.glb": "geometry.mixed_static_skinned",
            "bad_weights.glb": "skin.weights_normalized",
            "joint_out_of_range.glb": "skin.joint_range",
            "multiple_skins.glb": "skin.count",
            "too_many_joints.glb": "skin.joint_limit",
        }
        for fixture, rule_id in expected.items():
            with self.subTest(fixture=fixture):
                result, report = self.validate(fixture)
                self.assertEqual(result.returncode, 1, report)
                self.assertFalse(report["valid"])
                self.assertIn(rule_id, {finding["rule_id"] for finding in report["findings"]})
                self.assert_report_shape(report)

    def test_usage_error(self) -> None:
        result = subprocess.run(
            [sys.executable, str(VALIDATOR)],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 2)
        error = json.loads(result.stderr)
        self.assertEqual(error["error"], "usage")


if __name__ == "__main__":
    unittest.main(verbosity=2)
