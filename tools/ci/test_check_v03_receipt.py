#!/usr/bin/env python3
"""Contract tests for check_v03_receipt.py. Run as: python3 tools/ci/test_check_v03_receipt.py"""

from __future__ import annotations

import contextlib
import copy
import io
import json
import re
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
    sections["net_soak"]["required_tests"] = ["NetSoak"]
    sections["net_soak"]["discovered_tests"] = ["NetSoak"]
    sections["net_soak"]["skipped_tests"] = 0
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
        for section in valid_receipt()["sections"].values():
            artifact = self.base / section["evidence"]
            artifact.parent.mkdir(parents=True, exist_ok=True)
            artifact.write_text("{}\n", encoding="utf-8")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _evaluate(self, receipt: dict) -> dict:
        return tool.evaluate(receipt, self.base)

    def _write_obj(self, data, name: str = "receipt.json") -> Path:
        path = self.base / name
        path.write_text(json.dumps(data), encoding="utf-8")
        return path

    def _assert_baseline(self, section: str) -> None:
        verdict = self._evaluate(valid_receipt())
        self.assertEqual(_check(verdict, section)["verdict"], "pass", section)
        self.assertEqual(verdict["verdict"], "pass")

    def _expect(self, receipt: dict, section: str, expected: str) -> dict:
        verdict = self._evaluate(receipt)
        check = _check(verdict, section)
        self.assertEqual(check["verdict"], expected, check)
        if expected != "pass":
            self.assertNotEqual(verdict["verdict"], "pass")
        return check

    def test_valid_receipt_passes_overall_with_exit_zero(self) -> None:
        verdict = self._evaluate(valid_receipt())
        self.assertEqual(verdict["verdict"], "pass")
        self.assertEqual(verdict["schema"], "luminumbra.v03_acceptance_verdict.v1")
        self.assertEqual([c["id"] for c in verdict["checks"]], REQUIRED)
        path = self._write_obj(valid_receipt())
        code, out, err = _run([str(path)])
        self.assertEqual(code, 0, err)
        self.assertIn("pass render_qualification: evidence: logs/render_qualification.json", out)
        self.assertIn("overall: pass", out)

    def _absent_render_driver_section(self) -> dict:
        # Inspect the actual emission branch so this portable test catches a
        # driver regression even on hosts without PowerShell or a native client.
        script = (Path(__file__).resolve().parents[1] / "gates" /
                  "run-v03-acceptance.ps1").read_text(encoding="utf-8")
        render = script.split("# --- render_qualification", 1)[1].split(
            "# --- saved_world_lifecycle", 1)[0]
        branch = re.search(
            r'elseif \(-not \(Test-Path -LiteralPath \$renderScript -PathType Leaf\)\) '
            r'\{\s*([^\n]+)\s*\} else \{', render)
        self.assertIsNotNone(branch, "missing absent-driver branch")
        emitted = re.fullmatch(
            r'\$sections\["render_qualification"\] = New-Section '
            r'-Status "([a-z]+)" -Detail \$renderDeferredDetail', branch[1].strip())
        self.assertIsNotNone(emitted, "absent driver must emit an evidence-free section")
        detail = re.search(r'^\$renderDeferredDetail = "([^"\n]+)"$', script, re.M)
        self.assertIsNotNone(detail)
        self.assertIn("v0.3.1", detail[1])
        self.assertIn("issue #241", detail[1])
        self.assertIn("NEEDS-NATIVE-RUN", detail[1])
        self.assertIn("render qualification outstanding", detail[1])
        self.assertIn("cell by cell", detail[1])
        dry_run = script.split("if ($DryRun) {", 1)[1].split("    exit 0", 1)[0]
        self.assertRegex(
            dry_run,
            r'elseif \(-not \(Test-Path -LiteralPath \$renderScript -PathType Leaf\)\) '
            r'\{\s*Write-Host \("\[dry-run\] render_qualification: unevaluated; " '
            r'\+ \$renderDeferredDetail\)')
        return {"status": emitted[1], "evidence": "", "detail": detail[1]}

    def test_absent_render_driver_defers_and_never_passes_acceptance(self) -> None:
        receipt = valid_receipt()
        section = self._absent_render_driver_section()
        (self.base / receipt["sections"]["render_qualification"]["evidence"]).unlink()
        receipt["sections"]["render_qualification"] = section
        verdict_path = self.base / "verdict.json"
        code, out, err = _run([str(self._write_obj(receipt)),
                               "--verdict-out", str(verdict_path)])
        self.assertEqual(code, 3, err)
        self.assertIn("unevaluated render_qualification:", out)
        self.assertIn(section["detail"], out)
        self.assertIn("overall: unevaluated", out)
        verdict = json.loads(verdict_path.read_text(encoding="utf-8"))
        self.assertEqual(verdict["verdict"], "unevaluated")
        check = _check(verdict, "render_qualification")
        self.assertEqual(check["verdict"], "unevaluated")
        self.assertIn(section["detail"], check["detail"])
        self.assertTrue(all(c["verdict"] == "pass" for c in verdict["checks"]
                            if c["id"] != "render_qualification"))

    def test_deferred_render_does_not_hide_other_failures(self) -> None:
        receipt = valid_receipt()
        receipt["sections"]["render_qualification"] = self._absent_render_driver_section()
        receipt["sections"]["audio_off"]["status"] = "fail"
        code, out, err = _run([str(self._write_obj(receipt))])
        self.assertEqual(code, 1, err)
        self.assertIn("unevaluated render_qualification:", out)
        self.assertIn("issue #241", out)
        self.assertIn("fail audio_off:", out)
        self.assertIn("overall: fail", out)

    def test_missing_evidence_reports_each_reference_and_exits_one(self) -> None:
        receipt = valid_receipt()
        for name in ("audio_off", "net_soak"):
            (self.base / receipt["sections"][name]["evidence"]).unlink()
        verdict = self._evaluate(receipt)
        for name in ("audio_off", "net_soak"):
            check = _check(verdict, name)
            self.assertEqual(check["verdict"], "fail")
            self.assertEqual(check["detail"], f"missing evidence artifact: logs/{name}.json")
        code, out, _ = _run([str(self._write_obj(receipt))])
        self.assertEqual(code, 1)
        self.assertIn("missing evidence artifact: logs/audio_off.json", out)
        self.assertIn("missing evidence artifact: logs/net_soak.json", out)

    def test_evidence_traversal_is_rejected_even_for_existing_files(self) -> None:
        for reference in ("../receipt.json", "logs/../receipt.json", r"logs\..\receipt.json"):
            with self.subTest(reference=reference):
                receipt = valid_receipt()
                self._write_obj(receipt)
                receipt["sections"]["audio_off"]["evidence"] = reference
                check = self._expect(receipt, "audio_off", "fail")
                self.assertEqual(check["detail"], f"evidence traversal: {reference}")

    def test_absolute_evidence_is_allowed_only_inside_root(self) -> None:
        receipt = valid_receipt()
        section = receipt["sections"]["audio_off"]
        section["evidence"] = str(self.base / section["evidence"])
        self._expect(receipt, "audio_off", "pass")
        section["evidence"] = str(Path(__file__).resolve())
        check = self._expect(receipt, "audio_off", "fail")
        self.assertIn("evidence outside root:", check["detail"])

    def test_directory_evidence_is_not_a_regular_file(self) -> None:
        receipt = valid_receipt()
        receipt["sections"]["audio_off"]["evidence"] = "logs"
        check = self._expect(receipt, "audio_off", "fail")
        self.assertEqual(check["detail"], "evidence is not a regular file: logs")

    def test_evidence_symlink_escape_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as outside:
            outside_dir = Path(outside)
            (outside_dir / "artifact.json").write_text("{}", encoding="utf-8")
            for reference, target in (("escaped-file", outside_dir / "artifact.json"),
                                      ("escaped-dir", outside_dir)):
                try:
                    (self.base / reference).symlink_to(target, target_is_directory=target.is_dir())
                except (NotImplementedError, OSError) as error:
                    self.skipTest(f"symlinks unavailable: {error}")
                receipt = valid_receipt()
                evidence = reference if target.is_file() else reference + "/artifact.json"
                receipt["sections"]["audio_off"]["evidence"] = evidence
                check = self._expect(receipt, "audio_off", "fail")
                self.assertEqual(check["detail"], f"evidence outside root: {evidence}")

    def test_explicit_evidence_root_is_relative_to_receipt_directory(self) -> None:
        receipt = valid_receipt()
        receipt["evidence_root"] = "logs"
        for section in receipt["sections"].values():
            section["evidence"] = Path(section["evidence"]).name
        self.assertEqual(self._evaluate(receipt)["verdict"], "pass")
        code, out, _ = _run([str(self._write_obj(receipt))])
        self.assertEqual(code, 0, out)

    def test_malformed_evidence_root_is_invalid_input(self) -> None:
        for root in (None, False, [], ""):
            receipt = valid_receipt()
            receipt["evidence_root"] = root
            with self.assertRaisesRegex(tool.InputError, "evidence_root"):
                self._evaluate(receipt)
            self.assertEqual(_run([str(self._write_obj(receipt))])[0], 2)

    def test_absolute_evidence_roots_are_refused(self) -> None:
        for root in (str(self.base), r"C:\evidence", r"\evidence", r"C:evidence",
                     r"\\server\share\evidence"):
            with self.subTest(root=root):
                receipt = valid_receipt()
                receipt["evidence_root"] = root
                with self.assertRaisesRegex(
                    tool.InputError, "^malformed receipt: evidence_root must be relative$"
                ):
                    self._evaluate(receipt)
                code, _, err = _run([str(self._write_obj(receipt))])
                self.assertEqual(code, 2)
                self.assertEqual(err, "error: malformed receipt: evidence_root must be relative\n")

    def test_evidence_root_parent_components_are_refused_before_resolution(self) -> None:
        (self.base / "tools").mkdir()
        (self.base / "docs").mkdir()
        for root in ("tools/../docs", "docs/..", r"tools\..\docs", r"docs\..",
                     "tools/..\\docs", "..", "../" + self.base.name + "-outside"):
            with self.subTest(root=root):
                receipt = valid_receipt()
                receipt["evidence_root"] = root
                with self.assertRaisesRegex(
                    tool.InputError,
                    "^malformed receipt: evidence_root must not contain '\\.\\.' components$",
                ):
                    self._evaluate(receipt)
                code, _, err = _run([str(self._write_obj(receipt))])
                self.assertEqual(code, 2)
                self.assertEqual(
                    err, "error: malformed receipt: evidence_root must not contain '..' components\n"
                )

    def test_evidence_roots_resolving_outside_receipt_directory_are_refused(self) -> None:
        with tempfile.TemporaryDirectory() as outside:
            # Also reject a sibling whose name starts with the receipt directory's name.
            sibling = self.base.with_name(self.base.name + "-outside")
            sibling.mkdir()
            self.addCleanup(sibling.rmdir)
            try:
                (self.base / "escaped-root").symlink_to(outside, target_is_directory=True)
                (self.base / "escaped-sibling").symlink_to(sibling, target_is_directory=True)
            except (NotImplementedError, OSError) as error:
                self.skipTest(f"symlinks unavailable: {error}")
            for root in ("escaped-root", "escaped-sibling"):
                with self.subTest(root=root):
                    receipt = valid_receipt()
                    receipt["evidence_root"] = root
                    with self.assertRaisesRegex(
                        tool.InputError, "^malformed receipt: evidence_root outside receipt directory$"
                    ):
                        self._evaluate(receipt)
                    code, _, err = _run([str(self._write_obj(receipt))])
                    self.assertEqual(code, 2)
                    self.assertEqual(
                        err, "error: malformed receipt: evidence_root outside receipt directory\n"
                    )

    def test_evidence_root_symlink_within_receipt_directory_passes(self) -> None:
        try:
            (self.base / "root-alias").symlink_to(self.base / "logs", target_is_directory=True)
        except (NotImplementedError, OSError) as error:
            self.skipTest(f"symlinks unavailable: {error}")
        receipt = valid_receipt()
        receipt["evidence_root"] = "root-alias"
        for section in receipt["sections"].values():
            section["evidence"] = Path(section["evidence"]).name
        self.assertEqual(self._evaluate(receipt)["verdict"], "pass")

    def test_file_or_missing_evidence_root_is_invalid_input(self) -> None:
        file_root = self._write_obj({}, "file-root.json")
        for root in (file_root.name, "./" + file_root.name, "missing-root"):
            with self.subTest(root=root):
                receipt = valid_receipt()
                receipt["evidence_root"] = root
                for section in receipt["sections"].values():
                    section["evidence"] = "."
                with self.assertRaisesRegex(tool.InputError, "existing directory"):
                    self._evaluate(receipt)
                code, out, err = _run([str(self._write_obj(receipt))])
                self.assertEqual(code, 2)
                self.assertNotIn("overall: pass", out)
                self.assertIn("existing directory", err)

    def test_evidence_references_to_root_itself_fail(self) -> None:
        for reference in (".", "./", str(self.base), str(self.base / "logs" / "..")):
            with self.subTest(reference=reference):
                receipt = valid_receipt()
                receipt["sections"]["audio_off"]["evidence"] = reference
                check = self._expect(receipt, "audio_off", "fail")
                expected = "evidence traversal" if ".." in reference else "evidence resolves to root itself"
                self.assertEqual(check["detail"], f"{expected}: {reference}")
                self.assertEqual(_run([str(self._write_obj(receipt))])[0], 1)

    def test_root_self_via_relative_traversal_fails(self) -> None:
        for reference in ("logs/..", r"logs\..", "../" + self.base.name):
            with self.subTest(reference=reference):
                receipt = valid_receipt()
                receipt["sections"]["audio_off"]["evidence"] = reference
                check = self._expect(receipt, "audio_off", "fail")
                self.assertEqual(check["detail"], f"evidence traversal: {reference}")

    def test_symlink_resolving_to_root_itself_fails(self) -> None:
        try:
            (self.base / "root-alias").symlink_to(self.base, target_is_directory=True)
        except (NotImplementedError, OSError) as error:
            self.skipTest(f"symlinks unavailable: {error}")
        receipt = valid_receipt()
        receipt["sections"]["audio_off"]["evidence"] = "root-alias"
        check = self._expect(receipt, "audio_off", "fail")
        self.assertEqual(check["detail"], "evidence resolves to root itself: root-alias")

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

    def test_invalid_skipped_test_counts_are_refused(self) -> None:
        for skipped in (-1, True, False, 0.0, 1.5, "0", None):
            with self.subTest(skipped=skipped):
                receipt = valid_receipt()
                receipt["sections"]["net_soak"]["skipped_tests"] = skipped
                check = self._expect(receipt, "net_soak", "fail")
                self.assertEqual(check["detail"], "malformed skipped_tests")

    def test_passing_net_soak_requires_zero_skipped_tests(self) -> None:
        receipt = valid_receipt()
        self._expect(receipt, "net_soak", "pass")
        del receipt["sections"]["net_soak"]["skipped_tests"]
        check = self._expect(receipt, "net_soak", "fail")
        self.assertEqual(check["detail"], "missing skipped_tests")
        self.assertEqual(_run([str(self._write_obj(receipt))])[0], 1)
        receipt["sections"]["net_soak"]["skipped_tests"] = 1
        check = self._expect(receipt, "net_soak", "fail")
        self.assertEqual(check["detail"], "unexpected skipped tests: 1")

    def test_net_soak_requires_both_discovery_fields(self) -> None:
        for fields in (("required_tests",), ("discovered_tests",),
                       ("required_tests", "discovered_tests")):
            with self.subTest(fields=fields):
                receipt = valid_receipt()
                for field in fields:
                    del receipt["sections"]["net_soak"][field]
                check = self._expect(receipt, "net_soak", "fail")
                self.assertEqual(check["detail"], f"missing {fields[0]}")

    def test_net_soak_validates_discovery_fields(self) -> None:
        for field, bad in (("required_tests", []), ("required_tests", "NetSoak"),
                           ("required_tests", [1]), ("discovered_tests", None),
                           ("discovered_tests", "NetSoak"), ("discovered_tests", [True])):
            with self.subTest(field=field, bad=bad):
                receipt = valid_receipt()
                receipt["sections"]["net_soak"][field] = bad
                check = self._expect(receipt, "net_soak", "fail")
                self.assertEqual(check["detail"], "malformed required_tests")
        for discovered in ([], ["OtherTest"]):
            receipt = valid_receipt()
            receipt["sections"]["net_soak"]["discovered_tests"] = discovered
            check = self._expect(receipt, "net_soak", "fail")
            self.assertEqual(check["detail"], "missing named discovery: NetSoak")

    def test_unrun_net_soak_without_discovery_remains_unevaluated(self) -> None:
        receipt = valid_receipt()
        receipt["sections"]["net_soak"] = {"status": "unevaluated"}
        check = self._expect(receipt, "net_soak", "unevaluated")
        self.assertEqual(check["detail"], "section reported unevaluated")

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
        verdict = self._evaluate(receipt)
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
            others = [c for c in self._evaluate(receipt)["checks"] if c["id"] != name]
            self.assertTrue(all(c["verdict"] == "pass" for c in others))

    def test_fully_empty_sections_never_pass(self) -> None:
        receipt = {"schema": "luminumbra.v03_acceptance.v1", "sections": {}}
        verdict = self._evaluate(copy.deepcopy(receipt))
        self.assertEqual(verdict["verdict"], "unevaluated")
        self.assertEqual(len(verdict["checks"]), len(REQUIRED))
        for check in verdict["checks"]:
            self.assertEqual(check["verdict"], "unevaluated")


if __name__ == "__main__":
    unittest.main()
