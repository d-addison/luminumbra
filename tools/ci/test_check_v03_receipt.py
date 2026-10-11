#!/usr/bin/env python3
"""Contract tests for check_v03_receipt.py. Run as: python3 tools/ci/test_check_v03_receipt.py"""

from __future__ import annotations

import contextlib
import copy
import io
import json
import tempfile
import unittest
from pathlib import Path

import check_v03_receipt as tool

REQUIRED = [
    "render_qualification",
    "saved_world_lifecycle",
    "noui_load",
    "serial_load_20",
    "net_soak",
    "perf_pointer",
    "audio_off",
]


def valid_receipt() -> dict:
    sections = {}
    for name in REQUIRED:
        sections[name] = {"status": "pass", "evidence": f"logs/{name}.json"}
    sections["noui_load"]["required_tests"] = ["NoUiLoadsSavedWorld"]
    sections["noui_load"]["discovered_tests"] = ["NoUiLoadsSavedWorld", "OtherTest"]
    sections["noui_load"]["skipped_tests"] = 0
    return {
        "schema": "luminumbra.v03_acceptance.v1",
        "revision": "0123abcd",
        "sections": sections,
    }


def _check(verdict: dict, check_id: str) -> dict:
    matches = [check for check in verdict["checks"] if check["id"] == check_id]
    if len(matches) != 1:
        raise AssertionError(f"expected one check {check_id}, got {len(matches)}")
    return matches[0]


def _run(argv: list) -> tuple:
    out = io.StringIO()
    err = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = tool.main(argv)
    return code, out.getvalue(), err.getvalue()


class V03AcceptanceReceiptContract(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.base = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _write_obj(self, data, name: str = "receipt.json") -> Path:
        path = self.base / name
        path.write_text(json.dumps(data), encoding="utf-8")
        return path

    def _assert_baseline(self, section: str) -> None:
        verdict = tool.evaluate(valid_receipt())
        self.assertEqual(_check(verdict, section)["verdict"], "pass", section)
        self.assertEqual(verdict["verdict"], "pass")

    def _expect(self, receipt: dict, section: str, expected: str) -> dict:
        verdict = tool.evaluate(receipt)
        check = _check(verdict, section)
        self.assertEqual(check["verdict"], expected, check)
        if expected != "pass":
            self.assertNotEqual(verdict["verdict"], "pass")
        return check

    def test_valid_receipt_passes_overall_with_exit_zero(self) -> None:
        verdict = tool.evaluate(valid_receipt())
        self.assertEqual(verdict["verdict"], "pass")
        self.assertEqual(verdict["schema"], "luminumbra.v03_acceptance_verdict.v1")
        self.assertEqual([c["id"] for c in verdict["checks"]], REQUIRED)
        path = self._write_obj(valid_receipt())
        code, out, err = _run([str(path)])
        self.assertEqual(code, 0, err)
        self.assertIn("pass render_qualification: evidence: logs/render_qualification.json", out)
        self.assertIn("overall: pass", out)

    def test_unevaluated_exits_three(self) -> None:
        self._assert_baseline("audio_off")
        receipt = valid_receipt()
        del receipt["sections"]["audio_off"]
        code, out, _ = _run([str(self._write_obj(receipt))])
        self.assertEqual(code, 3)
        self.assertIn("unevaluated audio_off: section absent", out)
        self.assertIn("overall: unevaluated", out)

    def test_fail_exits_one(self) -> None:
        self._assert_baseline("net_soak")
        receipt = valid_receipt()
        receipt["sections"]["net_soak"]["status"] = "fail"
        code, out, _ = _run([str(self._write_obj(receipt))])
        self.assertEqual(code, 1)
        self.assertIn("overall: fail", out)

    def test_invalid_input_exits_two(self) -> None:
        self._assert_baseline("net_soak")
        code, _, err = _run([str(self.base / "missing.json")])
        self.assertEqual(code, 2)
        self.assertIn("error:", err)

        bad_json = self.base / "bad.json"
        bad_json.write_text("[not json", encoding="utf-8")
        self.assertEqual(_run([str(bad_json)])[0], 2)

        wrong_schema = valid_receipt()
        wrong_schema["schema"] = "luminumbra.v03_acceptance.v0"
        code, _, err = _run([str(self._write_obj(wrong_schema))])
        self.assertEqual(code, 2)
        self.assertIn("schema", err)

        not_object = self._write_obj(["sections"], "list.json")
        self.assertEqual(_run([str(not_object)])[0], 2)

        no_sections = valid_receipt()
        del no_sections["sections"]
        self.assertEqual(_run([str(self._write_obj(no_sections, "nosec.json"))])[0], 2)

        bad_sections = valid_receipt()
        bad_sections["sections"] = ["render_qualification"]
        self.assertEqual(_run([str(self._write_obj(bad_sections, "badsec.json"))])[0], 2)

    def test_verdict_out_is_written_and_parseable(self) -> None:
        self._assert_baseline("net_soak")
        out_path = self.base / "verdict.json"
        code, _, _ = _run([str(self._write_obj(valid_receipt())), "--verdict-out", str(out_path)])
        self.assertEqual(code, 0)
        text = out_path.read_text(encoding="utf-8")
        self.assertTrue(text.endswith("\n"))
        parsed = json.loads(text)
        self.assertEqual(parsed["verdict"], "pass")
        self.assertEqual(parsed["schema"], "luminumbra.v03_acceptance_verdict.v1")
        self.assertEqual(text, json.dumps(parsed, indent=2, sort_keys=True) + "\n")

    def test_skipped_status_fails(self) -> None:
        self._assert_baseline("serial_load_20")
        receipt = valid_receipt()
        receipt["sections"]["serial_load_20"]["status"] = "skipped"
        check = self._expect(receipt, "serial_load_20", "fail")
        self.assertEqual(check["detail"], "unexpected skip")

    def test_skipped_tests_fail(self) -> None:
        self._assert_baseline("noui_load")
        receipt = valid_receipt()
        receipt["sections"]["noui_load"]["skipped_tests"] = 2
        check = self._expect(receipt, "noui_load", "fail")
        self.assertEqual(check["detail"], "unexpected skipped tests: 2")

    def test_missing_named_discovery_fails(self) -> None:
        self._assert_baseline("noui_load")
        receipt = valid_receipt()
        receipt["sections"]["noui_load"]["discovered_tests"] = ["OtherTest"]
        check = self._expect(receipt, "noui_load", "fail")
        self.assertEqual(check["detail"], "missing named discovery: NoUiLoadsSavedWorld")

        receipt = valid_receipt()
        del receipt["sections"]["noui_load"]["discovered_tests"]
        self._expect(receipt, "noui_load", "fail")

    def test_fail_and_unevaluated_statuses(self) -> None:
        self._assert_baseline("perf_pointer")
        receipt = valid_receipt()
        receipt["sections"]["perf_pointer"]["status"] = "fail"
        self._expect(receipt, "perf_pointer", "fail")

        receipt = valid_receipt()
        receipt["sections"]["perf_pointer"]["status"] = "unevaluated"
        self._expect(receipt, "perf_pointer", "unevaluated")

    def test_unknown_status_fails(self) -> None:
        self._assert_baseline("perf_pointer")
        receipt = valid_receipt()
        receipt["sections"]["perf_pointer"]["status"] = "green"
        self._expect(receipt, "perf_pointer", "fail")

    def test_pass_without_evidence_is_unevaluated(self) -> None:
        self._assert_baseline("audio_off")
        receipt = valid_receipt()
        receipt["sections"]["audio_off"]["evidence"] = ""
        check = self._expect(receipt, "audio_off", "unevaluated")
        self.assertEqual(check["detail"], "pass without evidence")

        receipt = valid_receipt()
        del receipt["sections"]["audio_off"]["evidence"]
        self._expect(receipt, "audio_off", "unevaluated")

    def test_absent_section_is_unevaluated(self) -> None:
        self._assert_baseline("net_soak")
        receipt = valid_receipt()
        del receipt["sections"]["net_soak"]
        check = self._expect(receipt, "net_soak", "unevaluated")
        self.assertEqual(check["detail"], "section absent")

    def test_unknown_section_never_counts_as_evidence(self) -> None:
        self._assert_baseline("render_qualification")
        receipt = valid_receipt()
        receipt["sections"]["bonus_probe"] = {"status": "pass", "evidence": "x"}
        verdict = tool.evaluate(receipt)
        check = _check(verdict, "unknown_section:bonus_probe")
        self.assertEqual(check["verdict"], "unevaluated")
        self.assertEqual(check["detail"], "unknown section; not counted as evidence")
        self.assertEqual(verdict["verdict"], "unevaluated")

    def test_malformed_section_fails(self) -> None:
        self._assert_baseline("net_soak")
        receipt = valid_receipt()
        receipt["sections"]["net_soak"] = "pass"
        check = self._expect(receipt, "net_soak", "fail")
        self.assertEqual(check["detail"], "malformed section")

    def test_each_required_section_is_its_own_check(self) -> None:
        for name in REQUIRED:
            self._assert_baseline(name)
            receipt = valid_receipt()
            receipt["sections"][name]["status"] = "fail"
            self._expect(receipt, name, "fail")
            others = [c for c in tool.evaluate(receipt)["checks"] if c["id"] != name]
            self.assertTrue(all(c["verdict"] == "pass" for c in others))

    def test_fully_empty_sections_never_pass(self) -> None:
        receipt = {"schema": "luminumbra.v03_acceptance.v1", "sections": {}}
        verdict = tool.evaluate(copy.deepcopy(receipt))
        self.assertEqual(verdict["verdict"], "unevaluated")
        self.assertEqual(len(verdict["checks"]), len(REQUIRED))
        for check in verdict["checks"]:
            self.assertEqual(check["verdict"], "unevaluated")


if __name__ == "__main__":
    unittest.main()
