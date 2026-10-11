#!/usr/bin/env python3

import contextlib
import importlib.util
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from xml.sax.saxutils import escape


MODULE_PATH = Path(__file__).with_name("compare_ambient_receipts.py")
SPEC = importlib.util.spec_from_file_location("compare_ambient_receipts", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


BASE_FIELDS = {
    "seed": "424242",
    "ticks": "64",
    "ceiling": "4",
    "cpu_simd": "4",
    "wind_ctor": "aa11",
    "wind_evolved": "bb22",
    "weather_ctor": "cc33",
    "weather_evolved": "dd44",
    "aether_ctor": "ee55",
    "aether_evolved": "ff66",
}


def receipt_line(**overrides):
    fields = dict(BASE_FIELDS)
    fields.update(overrides)
    ordered = ["version=1"] + [f"{key}={fields[key]}" for key in BASE_FIELDS]
    return "AMBIENT_RECEIPT " + " ".join(ordered)


def write_junit(directory, name, system_out_lines=(), property_values=()):
    body = "\n".join(escape(line) for line in system_out_lines)
    properties = "".join(
        '<property name="ambient_receipt" value="{}"/>'.format(
            escape(value, {'"': "&quot;"})
        )
        for value in property_values
    )
    xml = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<testsuites><testsuite name="common_tests" tests="1" failures="0">'
        '<testcase name="EmitsConstructorAndEvolvedHashes" classname="AmbientDeterminismReceipt">'
        f"<properties>{properties}</properties>"
        "</testcase>"
        f"<system-out>{body}</system-out>"
        "</testsuite></testsuites>\n"
    )
    path = Path(directory) / name
    path.write_text(xml, encoding="utf-8")
    return str(path)


def run_main(argv):
    out = io.StringIO()
    err = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = MODULE.main(argv)
    return code, out.getvalue(), err.getvalue()


class CompareAmbientReceiptsTests(unittest.TestCase):
    def test_agree_when_only_cpu_simd_differs(self):
        with tempfile.TemporaryDirectory() as directory:
            lane_a = write_junit(
                directory,
                "a.xml",
                ["[  PASSED  ] 1 test.", receipt_line(cpu_simd="4")],
            )
            lane_b = write_junit(
                directory,
                "b.xml",
                [receipt_line(cpu_simd="2")],
            )
            code, out, err = run_main([f"avx2={lane_a}", f"scalar={lane_b}"])
        self.assertEqual(code, 0, err)
        document = json.loads(out)
        self.assertEqual(document["schema"], "luminumbra.ambient_receipt_comparison.v1")
        self.assertEqual(document["verdict"], "agree")
        self.assertEqual(document["mismatches"], [])
        self.assertEqual(document["labels"]["avx2"]["cpu_simd"], "4")
        self.assertEqual(document["labels"]["scalar"]["cpu_simd"], "2")

    def test_disagree_reports_the_mismatching_key(self):
        with tempfile.TemporaryDirectory() as directory:
            lane_a = write_junit(directory, "a.xml", [receipt_line()])
            lane_b = write_junit(directory, "b.xml", [receipt_line(wind_evolved="0badc0de")])
            code, out, err = run_main([f"a={lane_a}", f"b={lane_b}"])
        self.assertEqual(code, 1, err)
        document = json.loads(out)
        self.assertEqual(document["verdict"], "disagree")
        self.assertEqual(len(document["mismatches"]), 1)
        mismatch = document["mismatches"][0]
        self.assertEqual(mismatch["key"], "wind_evolved")
        self.assertEqual(mismatch["values"], {"a": "bb22", "b": "0badc0de"})

    def test_missing_receipt_is_an_input_error(self):
        with tempfile.TemporaryDirectory() as directory:
            lane_a = write_junit(directory, "a.xml", [receipt_line()])
            lane_b = write_junit(directory, "b.xml", ["no receipt printed here"])
            code, out, err = run_main([f"a={lane_a}", f"b={lane_b}"])
        self.assertEqual(code, 2)
        self.assertIn("no receipt", err)
        self.assertEqual(out, "")

    def test_two_different_receipts_in_one_file_is_an_input_error(self):
        with tempfile.TemporaryDirectory() as directory:
            lane = write_junit(
                directory,
                "a.xml",
                [receipt_line(), receipt_line(wind_ctor="0000")],
            )
            code, out, err = run_main([f"a={lane}"])
        self.assertEqual(code, 2)
        self.assertIn("multiple receipts", err)

    def test_duplicate_labels_are_an_input_error(self):
        with tempfile.TemporaryDirectory() as directory:
            lane_a = write_junit(directory, "a.xml", [receipt_line()])
            lane_b = write_junit(directory, "b.xml", [receipt_line()])
            code, _out, err = run_main([f"same={lane_a}", f"same={lane_b}"])
        self.assertEqual(code, 2)
        self.assertIn("duplicate label", err)

    def test_expect_labels_must_match_exactly(self):
        with tempfile.TemporaryDirectory() as directory:
            lane_a = write_junit(directory, "a.xml", [receipt_line()])
            lane_b = write_junit(directory, "b.xml", [receipt_line()])
            code, _out, err = run_main(
                [f"a={lane_a}", f"b={lane_b}", "--expect-labels", "a,b,c"]
            )
        self.assertEqual(code, 2)
        self.assertIn("do not match", err)

    def test_expect_labels_accepts_exact_set(self):
        with tempfile.TemporaryDirectory() as directory:
            lane_a = write_junit(directory, "a.xml", [receipt_line()])
            lane_b = write_junit(directory, "b.xml", [receipt_line()])
            code, out, err = run_main(
                [f"b={lane_b}", f"a={lane_a}", "--expect-labels", "a,b"]
            )
        self.assertEqual(code, 0, err)
        self.assertEqual(json.loads(out)["verdict"], "agree")

    def test_receipt_found_only_in_property_is_accepted(self):
        with tempfile.TemporaryDirectory() as directory:
            lane_a = write_junit(directory, "a.xml", [], [receipt_line()])
            lane_b = write_junit(directory, "b.xml", [receipt_line()])
            code, out, err = run_main([f"a={lane_a}", f"b={lane_b}"])
        self.assertEqual(code, 0, err)
        self.assertEqual(json.loads(out)["verdict"], "agree")

    def test_identical_line_in_output_and_property_is_one_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            lane = write_junit(directory, "a.xml", [receipt_line()], [receipt_line()])
            code, out, err = run_main([f"a={lane}"])
        self.assertEqual(code, 0, err)
        self.assertEqual(json.loads(out)["labels"]["a"]["wind_ctor"], "aa11")

    def test_non_version_one_receipt_is_an_input_error(self):
        with tempfile.TemporaryDirectory() as directory:
            lane = write_junit(
                directory,
                "a.xml",
                [receipt_line().replace("version=1", "version=2")],
            )
            code, _out, err = run_main([f"a={lane}"])
        self.assertEqual(code, 2)
        self.assertIn("version", err)

    def test_unreadable_file_is_an_input_error(self):
        with tempfile.TemporaryDirectory() as directory:
            missing = str(Path(directory) / "absent.xml")
            code, _out, err = run_main([f"a={missing}"])
        self.assertEqual(code, 2)
        self.assertIn("cannot read", err)


if __name__ == "__main__":
    unittest.main()
