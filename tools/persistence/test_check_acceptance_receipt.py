#!/usr/bin/env python3
"""Contract tests for check_acceptance_receipt.py. Run as: python3 tools/persistence/test_check_acceptance_receipt.py"""

from __future__ import annotations

import contextlib
import copy
import io
import json
import tempfile
import unittest
from pathlib import Path

import check_acceptance_receipt as tool

HEX_A = "a" * 64
HEX_B = "b" * 64
HEX_C = "c" * 64
HEX_D = "d" * 64
HEX_E = "e" * 64


def valid_receipt() -> dict:
    sessions = [
        {"name": "create", "kind": "create_edit_save", "exit_code": 0, "timed_out": False,
         "shutdown": {"schema": "luminumbra.shutdown.v1", "complete": True, "jobs_drained": True}},
        {"name": "restart", "kind": "restart_select_load", "exit_code": 0, "timed_out": False,
         "shutdown": {"schema": "luminumbra.shutdown.v1", "jobs_drained": True}},
        {"name": "seed2", "kind": "second_seed", "exit_code": 0, "timed_out": False,
         "shutdown": {"schema": "luminumbra.shutdown.v1", "complete": True, "jobs_drained": True}},
        {"name": "back", "kind": "switch_back", "exit_code": 0, "timed_out": False,
         "shutdown": {"schema": "luminumbra.shutdown.v1", "complete": True, "jobs_drained": True}},
    ]
    switches = []
    for index in range(6):
        world = "alpha" if index % 2 == 0 else "beta"
        switches.append({
            "index": index,
            "world": world,
            "capture_sha256": HEX_C if world == "alpha" else HEX_D,
            "g_buffer_probe": "ok",
        })
    return {
        "schema": "luminumbra.saved_world_acceptance.v1",
        "package": {"exe_sha256": HEX_A, "exe_name": "luminumbra.exe", "version_stamp": "0.3.0-rc1"},
        "fixture": {"tree_digest": HEX_B, "file_count": 12},
        "sessions": sessions,
        "save_receipts": [
            {"label": "before_edit", "tree_digest": HEX_B, "file_count": 12},
            {"label": "fixture_after_loads", "tree_digest": HEX_B, "file_count": 12},
            {"label": "after_save", "tree_digest": HEX_E, "file_count": 13},
            {"label": "after_reload", "tree_digest": HEX_E, "file_count": 13},
        ],
        "reload_changes": {"added": [], "removed": [], "changed": []},
        "fixture_changes": {"added": [], "removed": [], "changed": []},
        "switches": switches,
        "refusals": [
            {"variant": variant, "exit_code": 2 + index, "diagnostic": True, "timed_out": False}
            for index, variant in enumerate(tool.REQUIRED_REFUSAL_VARIANTS)
        ],
    }


def _verdict(receipt: dict, min_switches: int = 6) -> dict:
    return tool.evaluate(receipt, min_switches)


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


class AcceptanceReceiptContract(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.base = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _write_obj(self, data, name: str = "receipt.json") -> Path:
        path = self.base / name
        path.write_text(json.dumps(data), encoding="utf-8")
        return path

    def _assert_baseline(self, check_id: str) -> None:
        verdict = _verdict(valid_receipt())
        self.assertEqual(_check(verdict, check_id)["verdict"], "pass", check_id)
        self.assertEqual(verdict["verdict"], "pass")

    def _expect(self, receipt: dict, check_id: str, expected: str, min_switches: int = 6) -> None:
        verdict = _verdict(receipt, min_switches)
        self.assertEqual(_check(verdict, check_id)["verdict"], expected, _check(verdict, check_id))
        if expected != "pass":
            self.assertNotEqual(verdict["verdict"], "pass")

    # Overall behaviour and exit codes

    def test_valid_receipt_passes_overall_with_exit_zero(self) -> None:
        verdict = _verdict(valid_receipt())
        self.assertEqual(verdict["verdict"], "pass")
        self.assertEqual(verdict["schema"], "luminumbra.saved_world_acceptance_verdict.v1")
        self.assertEqual(verdict["min_switches"], 6)
        self.assertEqual(len(verdict["checks"]), 9)
        path = self._write_obj(valid_receipt())
        code, out, err = _run([str(path)])
        self.assertEqual(code, 0, err)
        self.assertIn("overall: pass", out)
        self.assertIn("pass package_identity:", out)

    def test_unevaluated_receipt_exits_three(self) -> None:
        self._assert_baseline("refusal_matrix")
        receipt = valid_receipt()
        del receipt["refusals"]
        path = self._write_obj(receipt)
        code, out, _ = _run([str(path)])
        self.assertEqual(code, 3)
        self.assertIn("unevaluated refusal_matrix: missing refusals", out)
        self.assertIn("overall: unevaluated", out)

    def test_failed_receipt_exits_one(self) -> None:
        self._assert_baseline("refusal_matrix")
        receipt = valid_receipt()
        receipt["refusals"][0]["exit_code"] = 0
        path = self._write_obj(receipt)
        code, out, _ = _run([str(path)])
        self.assertEqual(code, 1)
        self.assertIn("overall: fail", out)

    def test_invalid_input_exits_two(self) -> None:
        self._assert_baseline("refusal_matrix")
        code, _, err = _run([str(self.base / "missing.json")])
        self.assertEqual(code, 2)
        self.assertIn("error:", err)

        bad_json = self.base / "bad.json"
        bad_json.write_text("{not json", encoding="utf-8")
        code, _, err = _run([str(bad_json)])
        self.assertEqual(code, 2)
        self.assertIn("error:", err)

        wrong_schema = valid_receipt()
        wrong_schema["schema"] = "luminumbra.saved_world_acceptance.v9"
        code, _, err = _run([str(self._write_obj(wrong_schema))])
        self.assertEqual(code, 2)
        self.assertIn("schema", err)

        not_object = self._write_obj([1, 2, 3], "list.json")
        self.assertEqual(_run([str(not_object)])[0], 2)

        wrong_top = valid_receipt()
        wrong_top["sessions"] = {"not": "a list"}
        self.assertEqual(_run([str(self._write_obj(wrong_top, "top.json"))])[0], 2)

        self.assertEqual(_run([])[0], 2)

    def test_verdict_out_is_written_and_parseable(self) -> None:
        self._assert_baseline("refusal_matrix")
        path = self._write_obj(valid_receipt())
        out_path = self.base / "verdict.json"
        code, out, _ = _run([str(path), "--verdict-out", str(out_path)])
        self.assertEqual(code, 0)
        text = out_path.read_text(encoding="utf-8")
        self.assertTrue(text.endswith("\n"))
        parsed = json.loads(text)
        self.assertEqual(parsed["verdict"], "pass")
        self.assertEqual(parsed["schema"], "luminumbra.saved_world_acceptance_verdict.v1")
        self.assertEqual([c["id"] for c in parsed["checks"]][0], "package_identity")
        self.assertEqual(text, json.dumps(parsed, indent=2, sort_keys=True) + "\n")

    def test_min_switches_option(self) -> None:
        self._assert_baseline("switch_count")
        receipt = valid_receipt()
        self._expect(receipt, "switch_count", "pass", min_switches=6)
        self.assertEqual(_verdict(receipt, 4)["min_switches"], 4)
        self.assertEqual(_check(_verdict(receipt, 4), "switch_count")["verdict"], "pass")
        self.assertEqual(_check(_verdict(receipt, 7), "switch_count")["verdict"], "fail")

        path = self._write_obj(receipt)
        self.assertEqual(_run([str(path), "--min-switches", "7"])[0], 1)
        self.assertEqual(_run([str(path), "--min-switches", "5"])[0], 0)
        self.assertEqual(_run([str(path), "--min-switches", "-1"])[0], 2)

    # Check 1: package_identity

    def test_package_identity_checks(self) -> None:
        self._assert_baseline("package_identity")
        receipt = valid_receipt()
        del receipt["package"]
        self._expect(receipt, "package_identity", "unevaluated")
        self.assertEqual(_check(_verdict(receipt), "package_identity")["detail"], "missing package")

        receipt = valid_receipt()
        receipt["package"]["exe_sha256"] = "A" * 64
        self._expect(receipt, "package_identity", "fail")

        receipt = valid_receipt()
        receipt["package"]["exe_name"] = ""
        self._expect(receipt, "package_identity", "fail")

        receipt = valid_receipt()
        receipt["package"]["version_stamp"] = 7
        self._expect(receipt, "package_identity", "fail")

    # Check 2: fixture_identity

    def test_fixture_identity_checks(self) -> None:
        self._assert_baseline("fixture_identity")
        receipt = valid_receipt()
        del receipt["fixture"]
        self._expect(receipt, "fixture_identity", "unevaluated")

        receipt = valid_receipt()
        receipt["fixture"]["file_count"] = 0
        self._expect(receipt, "fixture_identity", "fail")

        receipt = valid_receipt()
        receipt["fixture"]["file_count"] = True
        self._expect(receipt, "fixture_identity", "fail")

        receipt = valid_receipt()
        receipt["fixture"]["tree_digest"] = "xyz"
        self._expect(receipt, "fixture_identity", "fail")

    # Check 3: sessions_present

    def test_sessions_present_checks(self) -> None:
        self._assert_baseline("sessions_present")
        receipt = valid_receipt()
        del receipt["sessions"]
        self._expect(receipt, "sessions_present", "unevaluated")

        receipt = valid_receipt()
        receipt["sessions"] = []
        self._expect(receipt, "sessions_present", "unevaluated")

        receipt = valid_receipt()
        receipt["sessions"] = [s for s in receipt["sessions"] if s["kind"] != "second_seed"]
        self._expect(receipt, "sessions_present", "fail")

        receipt = valid_receipt()
        receipt["sessions"][1], receipt["sessions"][2] = receipt["sessions"][2], receipt["sessions"][1]
        self._expect(receipt, "sessions_present", "fail")

        receipt = valid_receipt()
        receipt["sessions"].append({"name": "extra", "kind": "create_edit_save", "exit_code": 0,
                                    "timed_out": False, "shutdown": None})
        self._expect(receipt, "sessions_present", "pass")

    # Check 4: sessions_clean

    def test_sessions_clean_fails_on_timeout_and_exit_code(self) -> None:
        self._assert_baseline("sessions_clean")
        receipt = valid_receipt()
        receipt["sessions"][1]["timed_out"] = True
        self._expect(receipt, "sessions_clean", "fail")
        self.assertIn("restart", _check(_verdict(receipt), "sessions_clean")["detail"])

        receipt = valid_receipt()
        receipt["sessions"][2]["exit_code"] = 1
        self._expect(receipt, "sessions_clean", "fail")
        self.assertIn("seed2", _check(_verdict(receipt), "sessions_clean")["detail"])

    def test_sessions_clean_fails_on_shutdown_defects(self) -> None:
        self._assert_baseline("sessions_clean")
        receipt = valid_receipt()
        receipt["sessions"][0]["shutdown"]["jobs_drained"] = False
        self._expect(receipt, "sessions_clean", "fail")

        receipt = valid_receipt()
        receipt["sessions"][0]["shutdown"]["complete"] = False
        self._expect(receipt, "sessions_clean", "fail")

        receipt = valid_receipt()
        receipt["sessions"][0]["shutdown"]["schema"] = "luminumbra.shutdown.v0"
        self._expect(receipt, "sessions_clean", "fail")

    def test_sessions_clean_unevaluated_without_shutdown_record(self) -> None:
        self._assert_baseline("sessions_clean")
        receipt = valid_receipt()
        receipt["sessions"][3]["shutdown"] = None
        self._expect(receipt, "sessions_clean", "unevaluated")
        self.assertIn("back", _check(_verdict(receipt), "sessions_clean")["detail"])

    def test_sessions_clean_absent_is_unevaluated(self) -> None:
        self._assert_baseline("sessions_clean")
        receipt = valid_receipt()
        del receipt["sessions"]
        self._expect(receipt, "sessions_clean", "unevaluated")

    def test_sessions_clean_malformed_is_fail(self) -> None:
        self._assert_baseline("sessions_clean")
        receipt = valid_receipt()
        receipt["sessions"][0]["exit_code"] = "0"
        verdict = _verdict(receipt)
        self.assertEqual(_check(verdict, "sessions_clean")["verdict"], "fail")
        self.assertEqual(_check(verdict, "sessions_clean")["detail"], "malformed sessions")

    # Check 5: save_tree_changes

    def test_save_tree_changes_missing_labels_unevaluated(self) -> None:
        self._assert_baseline("save_tree_changes")
        for label in tool.REQUIRED_LABELS:
            receipt = valid_receipt()
            receipt["save_receipts"] = [e for e in receipt["save_receipts"] if e["label"] != label]
            self._expect(receipt, "save_tree_changes", "unevaluated")
            self.assertIn(label, _check(_verdict(receipt), "save_tree_changes")["detail"])

        receipt = valid_receipt()
        del receipt["save_receipts"]
        self._expect(receipt, "save_tree_changes", "unevaluated")

    def test_save_tree_changes_missing_change_dicts_unevaluated(self) -> None:
        self._assert_baseline("save_tree_changes")
        receipt = valid_receipt()
        del receipt["reload_changes"]
        self._expect(receipt, "save_tree_changes", "unevaluated")
        self.assertIn("reload_changes", _check(_verdict(receipt), "save_tree_changes")["detail"])

        receipt = valid_receipt()
        del receipt["fixture_changes"]
        self._expect(receipt, "save_tree_changes", "unevaluated")
        self.assertIn("fixture_changes", _check(_verdict(receipt), "save_tree_changes")["detail"])

    def test_save_tree_changes_does_not_require_edit_to_change_tree(self) -> None:
        # before_edit and after_save snapshot different worlds, so equal digests are fine.
        self._assert_baseline("save_tree_changes")
        receipt = valid_receipt()
        receipt["save_receipts"][2]["tree_digest"] = HEX_B
        receipt["save_receipts"][3]["tree_digest"] = HEX_B
        self._expect(receipt, "save_tree_changes", "pass")

    def test_save_tree_changes_primary_needs_files_after_save(self) -> None:
        self._assert_baseline("save_tree_changes")
        receipt = valid_receipt()
        receipt["save_receipts"][2]["file_count"] = 0
        self._expect(receipt, "save_tree_changes", "fail")

    def test_save_tree_changes_primary_only_world_info_allowed(self) -> None:
        self._assert_baseline("save_tree_changes")
        receipt = valid_receipt()
        receipt["save_receipts"][3]["tree_digest"] = HEX_D
        receipt["reload_changes"] = {"added": [], "removed": [], "changed": ["world_info.json"]}
        self.assertEqual(_check(_verdict(receipt), "save_tree_changes")["verdict"], "pass")

        receipt["reload_changes"]["changed"] = ["world_info.json", "worlds/a.lmr"]
        self._expect(receipt, "save_tree_changes", "fail")

        receipt["reload_changes"] = {"added": [], "removed": [], "changed": ["world_info.json "]}
        self._expect(receipt, "save_tree_changes", "fail")

    def test_save_tree_changes_primary_added_or_removed_fail(self) -> None:
        self._assert_baseline("save_tree_changes")
        receipt = valid_receipt()
        receipt["save_receipts"][3]["tree_digest"] = HEX_D
        receipt["reload_changes"] = {"added": ["extra.bin"], "removed": [], "changed": ["world_info.json"]}
        self._expect(receipt, "save_tree_changes", "fail")

        receipt["reload_changes"] = {"added": [], "removed": ["world_info.json"], "changed": []}
        self._expect(receipt, "save_tree_changes", "fail")

    def test_save_tree_changes_primary_equal_digests_need_empty_changes(self) -> None:
        self._assert_baseline("save_tree_changes")
        receipt = valid_receipt()
        receipt["reload_changes"]["changed"] = ["world_info.json"]
        self._expect(receipt, "save_tree_changes", "fail")

    def test_save_tree_changes_primary_differs_without_evidence_fail(self) -> None:
        self._assert_baseline("save_tree_changes")
        receipt = valid_receipt()
        receipt["save_receipts"][3]["tree_digest"] = HEX_D
        receipt["reload_changes"] = {"added": [], "removed": [], "changed": []}
        verdict = _verdict(receipt)
        self.assertEqual(_check(verdict, "save_tree_changes")["verdict"], "fail")
        self.assertIn("digest differs without change evidence", _check(verdict, "save_tree_changes")["detail"])

    def test_save_tree_changes_fixture_pair_rules(self) -> None:
        self._assert_baseline("save_tree_changes")
        receipt = valid_receipt()
        receipt["save_receipts"][1]["tree_digest"] = HEX_C
        receipt["fixture_changes"] = {"added": [], "removed": [], "changed": ["world_info.json"]}
        self._expect(receipt, "save_tree_changes", "pass")

        receipt["fixture_changes"] = {"added": [], "removed": [], "changed": []}
        verdict = _verdict(receipt)
        self.assertEqual(_check(verdict, "save_tree_changes")["verdict"], "fail")
        self.assertIn("fixture: digest differs without change evidence",
                      _check(verdict, "save_tree_changes")["detail"])

        receipt["fixture_changes"] = {"added": [], "removed": [], "changed": ["fixture/a.lmr"]}
        self._expect(receipt, "save_tree_changes", "fail")

        receipt["fixture_changes"] = {"added": ["x.bin"], "removed": [], "changed": ["world_info.json"]}
        self._expect(receipt, "save_tree_changes", "fail")

        receipt = valid_receipt()
        receipt["fixture_changes"] = {"added": [], "removed": [], "changed": ["world_info.json"]}
        self._expect(receipt, "save_tree_changes", "fail")

    def test_save_tree_changes_malformed_change_dicts_fail(self) -> None:
        self._assert_baseline("save_tree_changes")
        receipt = valid_receipt()
        receipt["reload_changes"] = {"added": "none", "removed": [], "changed": []}
        verdict = _verdict(receipt)
        self.assertEqual(_check(verdict, "save_tree_changes")["verdict"], "fail")
        self.assertEqual(_check(verdict, "save_tree_changes")["detail"], "malformed reload_changes")

        receipt = valid_receipt()
        receipt["fixture_changes"] = {"added": [], "removed": [], "changed": [3]}
        verdict = _verdict(receipt)
        self.assertEqual(_check(verdict, "save_tree_changes")["verdict"], "fail")
        self.assertEqual(_check(verdict, "save_tree_changes")["detail"], "malformed fixture_changes")

    # Check 6: switch_count

    def test_switch_count_checks(self) -> None:
        self._assert_baseline("switch_count")
        receipt = valid_receipt()
        del receipt["switches"]
        self._expect(receipt, "switch_count", "unevaluated")

        receipt = valid_receipt()
        receipt["switches"] = receipt["switches"][:5]
        self._expect(receipt, "switch_count", "fail")

        receipt = valid_receipt()
        receipt["switches"][0]["index"] = "zero"
        verdict = _verdict(receipt)
        self.assertEqual(_check(verdict, "switch_count")["verdict"], "fail")
        self.assertEqual(_check(verdict, "switch_count")["detail"], "malformed switches")

    # Check 7: switch_captures_identical

    def test_switch_captures_null_is_unevaluated(self) -> None:
        self._assert_baseline("switch_captures_identical")
        receipt = valid_receipt()
        receipt["switches"][3]["capture_sha256"] = None
        self._expect(receipt, "switch_captures_identical", "unevaluated")

    def test_switch_captures_absent_is_unevaluated(self) -> None:
        self._assert_baseline("switch_captures_identical")
        receipt = valid_receipt()
        del receipt["switches"]
        self._expect(receipt, "switch_captures_identical", "unevaluated")

    def test_switch_captures_differ_within_world_fail(self) -> None:
        self._assert_baseline("switch_captures_identical")
        receipt = valid_receipt()
        receipt["switches"][4]["capture_sha256"] = HEX_E
        verdict = _verdict(receipt)
        self.assertEqual(_check(verdict, "switch_captures_identical")["verdict"], "fail")
        self.assertIn("alpha", _check(verdict, "switch_captures_identical")["detail"])

    def test_switch_captures_group_per_world(self) -> None:
        self._assert_baseline("switch_captures_identical")
        receipt = valid_receipt()
        # alpha and beta have different captures from each other; that is allowed.
        self.assertNotEqual(receipt["switches"][0]["capture_sha256"],
                            receipt["switches"][1]["capture_sha256"])
        self.assertEqual(_check(_verdict(receipt), "switch_captures_identical")["verdict"], "pass")

    # Check 7b: switch_probes

    def test_switch_probes_baseline_and_unevaluated(self) -> None:
        self._assert_baseline("switch_probes")
        receipt = valid_receipt()
        receipt["switches"][2]["g_buffer_probe"] = None
        self._expect(receipt, "switch_probes", "unevaluated")
        self.assertIn("2", _check(_verdict(receipt), "switch_probes")["detail"])

        receipt = valid_receipt()
        del receipt["switches"][5]["g_buffer_probe"]
        self._expect(receipt, "switch_probes", "unevaluated")

        receipt = valid_receipt()
        del receipt["switches"]
        self._expect(receipt, "switch_probes", "unevaluated")

    def test_switch_probes_empty_string_fails(self) -> None:
        self._assert_baseline("switch_probes")
        receipt = valid_receipt()
        receipt["switches"][0]["g_buffer_probe"] = ""
        self._expect(receipt, "switch_probes", "fail")

    def test_switch_probes_non_string_is_malformed(self) -> None:
        self._assert_baseline("switch_probes")
        receipt = valid_receipt()
        receipt["switches"][0]["g_buffer_probe"] = 1
        verdict = _verdict(receipt)
        self.assertEqual(_check(verdict, "switch_probes")["verdict"], "fail")
        self.assertEqual(_check(verdict, "switch_probes")["detail"], "malformed switches")

    # Check 8: refusal_matrix

    def test_refusal_matrix_checks(self) -> None:
        self._assert_baseline("refusal_matrix")
        receipt = valid_receipt()
        del receipt["refusals"]
        self._expect(receipt, "refusal_matrix", "unevaluated")

        receipt = valid_receipt()
        receipt["refusals"] = []
        self._expect(receipt, "refusal_matrix", "unevaluated")

        receipt = valid_receipt()
        receipt["refusals"][1]["exit_code"] = 0
        self._expect(receipt, "refusal_matrix", "fail")

        receipt = valid_receipt()
        receipt["refusals"][0]["diagnostic"] = False
        self._expect(receipt, "refusal_matrix", "fail")

        receipt = valid_receipt()
        receipt["refusals"][0]["diagnostic"] = "yes"
        verdict = _verdict(receipt)
        self.assertEqual(_check(verdict, "refusal_matrix")["detail"], "malformed refusals")

        receipt = valid_receipt()
        receipt["refusals"][0]["timed_out"] = "no"
        verdict = _verdict(receipt)
        self.assertEqual(_check(verdict, "refusal_matrix")["detail"], "malformed refusals")

    def test_refusal_timed_out_fails(self) -> None:
        self._assert_baseline("refusal_matrix")
        receipt = valid_receipt()
        receipt["refusals"][4]["timed_out"] = True
        verdict = _verdict(receipt)
        self.assertEqual(_check(verdict, "refusal_matrix")["verdict"], "fail")
        self.assertIn("missing-preset", _check(verdict, "refusal_matrix")["detail"])

    def test_refusal_timed_out_key_is_optional(self) -> None:
        receipt = valid_receipt()
        for refusal in receipt["refusals"]:
            del refusal["timed_out"]
        self._expect(receipt, "refusal_matrix", "pass")

    def test_refusal_missing_required_variant_unevaluated(self) -> None:
        self._assert_baseline("refusal_matrix")
        receipt = valid_receipt()
        receipt["refusals"] = [r for r in receipt["refusals"] if r["variant"] != "truncated-region"]
        verdict = _verdict(receipt)
        self.assertEqual(_check(verdict, "refusal_matrix")["verdict"], "unevaluated")
        self.assertEqual(_check(verdict, "refusal_matrix")["detail"], "missing variants: truncated-region")

    def test_refusal_old_variant_names_are_not_accepted(self) -> None:
        receipt = valid_receipt()
        receipt["refusals"] = [
            {"variant": "missing_manifest", "exit_code": 2, "diagnostic": True, "timed_out": False},
        ]
        verdict = _verdict(receipt)
        self.assertEqual(_check(verdict, "refusal_matrix")["verdict"], "unevaluated")
        self.assertIn("obsolete-metadata", _check(verdict, "refusal_matrix")["detail"])

    def test_refusal_failure_beats_missing_variants(self) -> None:
        receipt = valid_receipt()
        receipt["refusals"] = [
            {"variant": "obsolete-metadata", "exit_code": 0, "diagnostic": True, "timed_out": False},
        ]
        verdict = _verdict(receipt)
        self.assertEqual(_check(verdict, "refusal_matrix")["verdict"], "fail")
        self.assertIn("missing variants", _check(verdict, "refusal_matrix")["detail"])

    def test_refusal_variants_option_overrides_required_list(self) -> None:
        receipt = valid_receipt()
        receipt["refusals"] = [
            {"variant": "alpha", "exit_code": 2, "diagnostic": True},
            {"variant": "beta", "exit_code": 3, "diagnostic": True},
        ]
        verdict = tool.evaluate(receipt, 6, ("alpha", "beta"))
        self.assertEqual(_check(verdict, "refusal_matrix")["verdict"], "pass")
        verdict = tool.evaluate(receipt, 6, ("alpha", "beta", "gamma"))
        self.assertEqual(_check(verdict, "refusal_matrix")["detail"], "missing variants: gamma")

        path = self._write_obj(receipt)
        self.assertEqual(_run([str(path), "--refusal-variants", "alpha,beta"])[0], 0)
        code, out, _ = _run([str(path), "--refusal-variants", "alpha, beta ,gamma"])
        self.assertEqual(code, 3)
        self.assertIn("missing variants: gamma", out)
        self.assertEqual(_run([str(path)])[0], 3)
        self.assertEqual(_run([str(path), "--refusal-variants", " , "])[0], 2)

    def test_every_check_id_is_present_in_order(self) -> None:
        ids = [check["id"] for check in _verdict(valid_receipt())["checks"]]
        self.assertEqual(ids, [
            "package_identity", "fixture_identity", "sessions_present", "sessions_clean",
            "save_tree_changes", "switch_count", "switch_captures_identical", "switch_probes",
            "refusal_matrix",
        ])

    def test_fully_absent_optional_fields_never_pass(self) -> None:
        receipt = {"schema": "luminumbra.saved_world_acceptance.v1"}
        verdict = _verdict(copy.deepcopy(receipt))
        self.assertEqual(verdict["verdict"], "unevaluated")
        for check in verdict["checks"]:
            self.assertEqual(check["verdict"], "unevaluated", check["id"])


if __name__ == "__main__":
    unittest.main()
