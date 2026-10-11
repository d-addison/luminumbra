#!/usr/bin/env python3

import contextlib
import importlib.util
import io
import sys
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("slowest_tests.py")
SPEC = importlib.util.spec_from_file_location("slowest_tests", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


JUNIT_TEMPLATE = """<?xml version="1.0" encoding="UTF-8"?>
<Site>
  <Testing>
{cases}
  </Testing>
</Site>
"""


def write_report(directory: Path, cases: str) -> Path:
    path = directory / "ctest.junit.xml"
    path.write_text(JUNIT_TEMPLATE.format(cases=cases), encoding="utf-8")
    return path


def run_main(*args: str):
    out = io.StringIO()
    err = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = MODULE.main(list(args))
    return code, out.getvalue(), err.getvalue()


class SlowestTestsTests(unittest.TestCase):
    def test_orders_slowest_first_with_name_tiebreak(self):
        with tempfile.TemporaryDirectory() as directory:
            report = write_report(
                Path(directory),
                '<testcase name="B.Two" time="1.5"/>'
                '<testcase name="A.One" time="3.0"/>'
                '<testcase name="C.Three" time="1.5"/>',
            )
            summary = MODULE.parse_report(report)
        self.assertEqual(
            [t.name for t in summary.timings],
            ["A.One", "B.Two", "C.Three"],
        )

    def test_count_limits_rows_but_total_covers_all_tests(self):
        with tempfile.TemporaryDirectory() as directory:
            report = write_report(
                Path(directory),
                "".join(
                    f'<testcase name="T{i}" time="{i}.0"/>' for i in range(5)
                ),
            )
            code, out, _ = run_main(str(report), "--count", "2")
        self.assertEqual(code, 0)
        self.assertIn("### Slowest 2 tests", out)
        self.assertIn("| 1 | 4.0 | T4 |", out)
        self.assertIn("| 2 | 3.0 | T3 |", out)
        self.assertNotIn("T2", out)
        self.assertIn("Total: 5 tests, 10.0 s; 0 skipped, 0 failed", out)

    def test_summary_file_is_appended_not_overwritten(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            report = write_report(base, '<testcase name="X" time="2.0"/>')
            summary_path = base / "summary.md"
            summary_path.write_text("existing content\n", encoding="utf-8")
            code, _, _ = run_main(
                str(report), "--summary-file", str(summary_path)
            )
            text = summary_path.read_text(encoding="utf-8")
        self.assertEqual(code, 0)
        self.assertTrue(text.startswith("existing content\n"))
        self.assertIn("### Slowest 1 tests", text)
        self.assertIn("| 1 | 2.0 | X |", text)

    def test_missing_report_exits_zero_with_stderr_note(self):
        with tempfile.TemporaryDirectory() as directory:
            missing = Path(directory) / "absent.xml"
            summary_path = Path(directory) / "summary.md"
            code, out, err = run_main(
                str(missing), "--summary-file", str(summary_path)
            )
            wrote_summary = summary_path.exists()
        self.assertEqual(code, 0)
        self.assertEqual(out, "")
        self.assertIn("missing", err)
        self.assertFalse(wrote_summary)

    def test_malformed_xml_exits_zero_with_stderr_note(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "ctest.junit.xml"
            path.write_text("<Site><Testing><testcase", encoding="utf-8")
            code, out, err = run_main(str(path))
        self.assertEqual(code, 0)
        self.assertEqual(out, "")
        self.assertIn("could not parse", err)

    def test_missing_or_invalid_time_counts_as_zero(self):
        with tempfile.TemporaryDirectory() as directory:
            report = write_report(
                Path(directory),
                '<testcase name="NoTime"/>'
                '<testcase name="BadTime" time="soon"/>'
                '<testcase name="NegTime" time="-4"/>'
                '<testcase name="Real" time="0.25"/>',
            )
            summary = MODULE.parse_report(report)
        seconds = {t.name: t.seconds for t in summary.timings}
        self.assertEqual(seconds["NoTime"], 0.0)
        self.assertEqual(seconds["BadTime"], 0.0)
        self.assertEqual(seconds["NegTime"], 0.0)
        self.assertEqual(seconds["Real"], 0.25)
        self.assertEqual(summary.timings[0].name, "Real")

    def test_pipe_characters_in_names_are_escaped(self):
        with tempfile.TemporaryDirectory() as directory:
            report = write_report(
                Path(directory),
                '<testcase name="Suite.a|b" time="1.0"/>',
            )
            code, out, _ = run_main(str(report))
        self.assertEqual(code, 0)
        self.assertIn("| 1 | 1.0 | Suite.a\\|b |", out)
        self.assertNotIn("Suite.a|b", out)

    def test_skipped_and_failed_cases_are_counted(self):
        with tempfile.TemporaryDirectory() as directory:
            report = write_report(
                Path(directory),
                '<testcase name="Ok" time="1.0"/>'
                '<testcase name="Skip" time="0.5"><skipped/></testcase>'
                '<testcase name="Fail" time="0.5"><failure message="x"/></testcase>'
                '<testcase name="Err" time="0.5"><error message="y"/></testcase>',
            )
            code, out, _ = run_main(str(report))
        self.assertEqual(code, 0)
        self.assertIn("Total: 4 tests, 2.5 s; 1 skipped, 2 failed", out)


if __name__ == "__main__":
    unittest.main()
