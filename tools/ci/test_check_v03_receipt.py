#!/usr/bin/env python3
"""Contract tests for check_v03_receipt.py. Run as: python3 tools/ci/test_check_v03_receipt.py"""

from __future__ import annotations

import contextlib
import copy
import io
import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
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


def audio_run(playback: bool) -> dict:
    volumes = {"master": 0.8, "music": 0.6, "sfx": 0.4}
    return {"exit_code": 0, "timed_out": False, "state": {
        "schema": "luminumbra.runtime_state.v1",
        "audio_acceptance": {
            "schema": "luminumbra.audio_acceptance.v1",
            "resolution": {"no_audio_requested": not playback,
                           "enable_audio_requested": not playback,
                           "requested_mode": "device" if playback else "null",
                           "resolved_mode": "device" if playback else "null",
                           "reason": "default" if playback else "flag"},
            "backend": {"playback_enabled": playback, "engine_initialized": playback,
                        "device_started": playback,
                        "routing": {key: playback for key in ("sfx", "ambient", "events", "ui")}},
            "saved_volumes": {"loaded": volumes.copy(), "in_effect": volumes.copy(),
                              "unchanged": True,
                              "playback_gains": volumes.copy() if playback else None},
            "controls": {"observed": True, "present": 3,
                         "disabled_count": 0 if playback else 3, "match_playback": True},
            "telemetry": {"opt_in_requested": False, "enabled": False,
                          "retained_requests": 0, "opt_in_only": True},
            "no_audio_precedence": None if playback else True,
            "controls_disabled": not playback,
        }}}


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
    sections["audio_off"]["runtime_snapshots"] = {
        "ordinary": audio_run(True), "no_audio": audio_run(False),
        "no_audio_reversed": audio_run(False)}
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

    def _audio_refuses(self, mutate, message: str, role: str = "ordinary") -> None:
        receipt = valid_receipt()
        audio = receipt["sections"]["audio_off"]["runtime_snapshots"][role]["state"]["audio_acceptance"]
        mutate(audio)
        check = self._expect(receipt, "audio_off", "fail")
        self.assertIn(message, check["detail"])

    def test_audio_stale_default_off_only_refuses(self) -> None:
        receipt = valid_receipt()
        receipt["sections"]["audio_off"] = {
            "status": "pass", "evidence": "logs/audio_off.json",
            "default_null_playback_with_saved_volume": True,
            "no_audio_precedence": True, "controls_disabled": True}
        check = self._expect(receipt, "audio_off", "fail")
        self.assertIn("stale default-off-only", check["detail"])

    def test_audio_missing_object_refuses(self) -> None:
        receipt = valid_receipt()
        del receipt["sections"]["audio_off"]["runtime_snapshots"]["ordinary"]["state"]["audio_acceptance"]
        check = self._expect(receipt, "audio_off", "fail")
        self.assertIn("audio_acceptance: missing key", check["detail"])

    def test_audio_false_started_device_refuses(self) -> None:
        self._audio_refuses(lambda a: a["backend"].update(device_started=False), "device_started")

    def test_audio_wrong_type_refuses(self) -> None:
        self._audio_refuses(lambda a: a["backend"].update(device_started=1), "device_started")

    def test_audio_wrong_version_refuses(self) -> None:
        self._audio_refuses(lambda a: a.update(schema="luminumbra.audio_acceptance.v2"), "schema")

    def test_audio_telemetry_without_opt_in_refuses(self) -> None:
        self._audio_refuses(lambda a: a["telemetry"].update(enabled=True),
                            "without opt-in", "no_audio")

    def test_audio_controls_not_observed_refuses(self) -> None:
        self._audio_refuses(lambda a: a["controls"].update(observed=False), "controls.observed")

    def test_audio_volumes_changed_refuses(self) -> None:
        self._audio_refuses(lambda a: a["saved_volumes"]["in_effect"].update(master=0.5),
                            "saved volumes changed", "no_audio")

    def test_audio_volumes_changed_across_runs_refuses(self) -> None:
        def change(audio):
            for field in ("loaded", "in_effect"):
                audio["saved_volumes"][field]["master"] = 0.5
        self._audio_refuses(change, "saved volumes changed", "no_audio_reversed")

    def test_audio_suppression_requires_explicit_null_gains(self) -> None:
        self._audio_refuses(lambda a: a["saved_volumes"].update(playback_gains={}),
                            "expected explicit null", "no_audio")

    def test_audio_failed_partial_or_timed_out_sessions_refuse(self) -> None:
        for role in ("ordinary", "no_audio", "no_audio_reversed"):
            for fields in ({"exit_code": 2}, {"timed_out": True}, {"state": None},
                           {"state": {"schema": "luminumbra.runtime_state.v2"}}):
                with self.subTest(role=role, fields=fields):
                    receipt = valid_receipt()
                    receipt["sections"]["audio_off"]["runtime_snapshots"][role].update(fields)
                    self._expect(receipt, "audio_off", "fail")
        for snapshots in (None, [], {}, {"ordinary": {}}):
            with self.subTest(snapshots=snapshots):
                receipt = valid_receipt()
                receipt["sections"]["audio_off"]["runtime_snapshots"] = snapshots
                self._expect(receipt, "audio_off", "fail")

    def test_audio_flag_precedence_and_truthful_controls(self) -> None:
        for role in ("ordinary", "no_audio", "no_audio_reversed"):
            for group, field in (("resolution", "resolved_mode"), ("resolution", "requested_mode"),
                                 ("resolution", "reason"), ("resolution", "no_audio_requested"),
                                 ("resolution", "enable_audio_requested"),
                                 ("backend", "playback_enabled"), ("backend", "engine_initialized"),
                                 ("controls", "match_playback"), ("controls", "disabled_count")):
                with self.subTest(role=role, group=group, field=field):
                    self._audio_refuses(lambda a: a[group].update({field: "unknown"}),
                                        f"{group}.{field}", role)
            for route in ("sfx", "ambient", "events", "ui"):
                with self.subTest(role=role, route=route):
                    self._audio_refuses(lambda a: a["backend"]["routing"].update(
                        {route: role != "ordinary"}), f"routing.{route}", role)

    def test_audio_every_required_key_and_unknown_observation_refuses(self) -> None:
        def paths(obj, prefix=()):
            for key, value in obj.items():
                yield prefix + (key,)
                if isinstance(value, dict):
                    yield from paths(value, prefix + (key,))
        baseline = valid_receipt()
        snapshots = baseline["sections"]["audio_off"]["runtime_snapshots"]
        # Fixed v1 fixture: visit every object and leaf, including run metadata.
        for path in paths(snapshots):
            for mutation in ("missing", "null", "wrong_type"):
                receipt = copy.deepcopy(baseline)
                parent = receipt["sections"]["audio_off"]["runtime_snapshots"]
                for key in path[:-1]:
                    parent = parent[key]
                original = parent[path[-1]]
                if original is None and mutation == "null":
                    continue  # These two explicit null fields are required observations.
                with self.subTest(path=path, mutation=mutation):
                    if mutation == "missing":
                        del parent[path[-1]]
                    else:
                        parent[path[-1]] = None if mutation == "null" else []
                    self._expect(receipt, "audio_off", "fail")

    def test_audio_unknown_key_in_every_schema_object_refuses(self) -> None:
        def objects(obj, prefix=()):
            yield prefix, obj
            for key, value in obj.items():
                if isinstance(value, dict):
                    yield from objects(value, prefix + (key,))
        baseline = valid_receipt()
        for role in ("ordinary", "no_audio", "no_audio_reversed"):
            audio = baseline["sections"]["audio_off"]["runtime_snapshots"][role]["state"][
                "audio_acceptance"]
            visited = 0
            for path, _ in objects(audio):
                visited += 1
                receipt = copy.deepcopy(baseline)
                target = receipt["sections"]["audio_off"]["runtime_snapshots"][role]["state"][
                    "audio_acceptance"]
                for key in path:
                    target = target[key]
                target["unexpected_observation"] = True
                with self.subTest(role=role, path=path):
                    check = self._expect(receipt, "audio_off", "fail")
                    self.assertIn("unexpected_observation: unknown key", check["detail"])
            # Every v1 audio object: top level, resolution, backend, routing, saved_volumes,
            # loaded, in_effect, controls, telemetry, plus playback_gains when playing.
            self.assertEqual(visited, 10 if role == "ordinary" else 9)

    def test_audio_volume_boundaries_and_nonfinite_refuse(self) -> None:
        for level in (True, "0.5", 0, -1, 1.01, float("nan"), float("inf"), 10**400):
            with self.subTest(level=level):
                self._audio_refuses(lambda a: a["saved_volumes"]["loaded"].update(master=level),
                                    "finite nonzero level")
        self._audio_refuses(lambda a: a["saved_volumes"]["playback_gains"].update(master=0.3),
                            "differs from saved volumes")

    def test_audio_telemetry_types_retention_and_opt_in(self) -> None:
        for values, message in (({"opt_in_only": False}, "opt_in_only"),
                                ({"enabled": "false"}, "enabled: expected boolean"),
                                ({"opt_in_requested": 1}, "opt_in_requested: expected boolean"),
                                ({"retained_requests": True}, "expected nonnegative integer"),
                                ({"retained_requests": -1}, "expected nonnegative integer"),
                                ({"retained_requests": 1}, "without opt-in")):
            with self.subTest(values=values):
                self._audio_refuses(lambda a: a["telemetry"].update(values), message, "no_audio")
        self._audio_refuses(lambda a: a["telemetry"].update(opt_in_requested=True),
                            "ordinary startup must not opt in")
        receipt = valid_receipt()
        receipt["sections"]["audio_off"]["runtime_snapshots"]["no_audio"]["state"][
            "audio_acceptance"]["telemetry"].update(
                opt_in_requested=True, enabled=True, retained_requests=2)
        self._expect(receipt, "audio_off", "pass")

    def test_isolated_entrypoint_from_other_directory(self) -> None:
        result = subprocess.run([sys.executable, "-I", "-B", str(Path(__file__).resolve()),
                                 "V03AcceptanceReceiptContract.test_valid_receipt_passes_overall_with_exit_zero"],
                                cwd=self.base, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_audio_driver_collects_observed_sessions_and_both_flag_orders(self) -> None:
        script = (Path(__file__).resolve().parents[1] / "gates" /
                  "run-v03-acceptance.ps1").read_text(encoding="utf-8")
        audio = script.split("# --- audio_off", 1)[1].split("} catch {\n    $orchestratorError", 1)[0]
        self.assertIn('name = "ordinary"; flags = @()', audio)
        self.assertIn('name = "no_audio"; flags = @("--enable-audio", "--no-audio")', audio)
        self.assertIn('name = "no_audio_reversed"; flags = @("--no-audio", "--enable-audio")', audio)
        self.assertIn(') + @($session.flags)', audio)
        self.assertIn('Invoke-ClientSession -Exe $exePath -ArgumentList $audioArgs', audio)
        self.assertIn('"last-known-runtime.json"', audio)
        self.assertIn('state = $audioState', audio)
        self.assertIn('exit_code = [int]$audioRun.exit_code', audio)
        self.assertIn('timed_out = [bool]$audioRun.timed_out', audio)
        self.assertIn('-Extra @{ runtime_snapshots = $audioSnapshots }', audio)
        self.assertNotIn('default_null_playback_with_saved_volume', audio)
        # Non-Windows hosts never qualify: a clean collection there is unevaluated, not pass.
        self.assertIn('$audioNativeHost = ($PSVersionTable.PSEdition -eq "Desktop") -or '
                      '($IsWindows -eq $true)', audio)
        self.assertRegex(audio, r'\} elseif \(-not \$audioNativeHost\) \{\s*(#[^\n]*\s*)?'
                                r'\$audioStatus = "unevaluated"')
        self.assertLess(audio.index('$audioProblems.Count -gt 0'), audio.index('-not $audioNativeHost'))

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


class FarBudgetReceiptContract(unittest.TestCase):
    def setUp(self):
        self.storage = tempfile.TemporaryDirectory()
        self.addCleanup(self.storage.cleanup)
        self.root = Path(self.storage.name)
        self.budget = tool._budget_tools()
        from test_far_budget import receipt_evidence, synthetic_receipt_measurements
        matrix, self.approval, section, self.index, checked = receipt_evidence(self.root)
        self.measurements = synthetic_receipt_measurements(matrix, checked)
        self.measurements.__enter__()
        self.addCleanup(self.measurements.__exit__, None, None, None)
        self.receipt = valid_receipt()
        self.receipt.update(schema=tool.INPUT_SCHEMA_V032, release="v0.3.2", revision="1" * 40)
        self.receipt["sections"]["far_budget"] = section
        for name, item in self.receipt["sections"].items():
            if name != "far_budget":
                path = self.root / item["evidence"]
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("{}\n", encoding="utf-8")

    def evaluate(self, **options):
        return tool.evaluate(self.receipt, self.root, budget_approval=self.approval,
                             expected_revision="1" * 40, require_version="v0.3.2", **options)

    def assert_refused(self):
        verdict = self.evaluate()
        self.assertNotEqual(verdict["verdict"], "pass", verdict)
        self.assertNotEqual(_check(verdict, "far_budget")["verdict"], "pass")

    def test_missing_budget_evidence_cannot_pass(self):
        self.assertEqual(self.evaluate()["verdict"], "pass")
        section = self.receipt["sections"].pop("far_budget")
        self.assert_refused()
        self.receipt["sections"]["far_budget"] = section
        for status in ("pass", "fail", "skipped", "unevaluated", "unknown"):
            for evidence in (None, "", "missing.json"):
                self.receipt["sections"]["far_budget"] = dict(section, status=status, evidence=evidence)
                with self.subTest(status=status, evidence=evidence):
                    self.assert_refused()
        self.receipt["sections"]["far_budget"] = section
        self.approval = None
        self.assert_refused()

    def test_profile_or_revision_mismatch_fails(self):
        for key, values in (("release", (None, "v0.3.1")),
                            ("revision", (None, "", "2" * 40))):
            previous = self.receipt[key]
            for value in values:
                self.receipt[key] = value
                with self.subTest(key=key, value=value):
                    if value is None and key == "revision":
                        with self.assertRaises(tool.InputError):
                            self.evaluate()
                    else:
                        self.assert_refused()
            self.receipt[key] = previous
        self.receipt["sections"]["far_budget"]["profile_id"] = "legacy"
        self.assert_refused()

    def test_single_failed_cell_blocks_acceptance(self):
        self.index["cells"][-1]["state"] = "fail"
        (self.root / "index.json").write_bytes(self.budget.canonical(self.index))
        self.assert_refused()

    def test_legacy_reader_and_release_guard_are_independent(self):
        legacy = valid_receipt()
        self.assertEqual(tool.evaluate(legacy, self.root)["verdict"], "pass")
        with self.assertRaisesRegex(tool.InputError, "version 2 receipt"):
            tool.evaluate(legacy, self.root, require_version="v0.3.2")
        for options in ({"budget_approval": self.approval}, {"expected_revision": "1" * 40}):
            with self.subTest(options=options), self.assertRaises(tool.InputError):
                tool.evaluate(legacy, self.root, **options)
        with self.assertRaises(tool.InputError):
            tool.evaluate(dict(legacy, release="v0.3.2"), self.root)
        for options in ({}, {"budget_approval": self.approval}, {"expected_revision": "1" * 40}):
            verdict = tool.evaluate(self.receipt, self.root, **options)
            self.assertEqual(verdict["verdict"], "fail")

    def test_cli_requires_independent_authority_and_never_launches(self):
        receipt_path = self.root / "receipt.json"
        receipt_path.write_text(json.dumps(self.receipt), encoding="utf-8")
        code, out, _ = _run([str(receipt_path), "--require-version", "v0.3.2"])
        self.assertEqual(code, tool.EXIT_FAIL, out)
        for payload in (b'{', b'[]', b'{"schema":1,"schema":2}', b''):
            approval_path = self.root / "approval.json"
            approval_path.write_bytes(payload)
            code, out, err = _run([str(receipt_path), "--budget-approval", str(approval_path),
                                   "--expected-revision", "1" * 40])
            self.assertNotEqual(code, tool.EXIT_PASS, out + err)
        result = subprocess.run([sys.executable, "-I", "-B", str(Path(tool.__file__)),
                                 str(receipt_path), "--require-version", "v0.3.2"],
                                cwd=self.root, capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, tool.EXIT_FAIL, result.stderr)

    def test_invalid_or_unreadable_source_becomes_failed_verdict(self):
        for error in (PermissionError("denied"), ValueError("partial"), KeyError("missing"),
                      TypeError("wrong type"), AttributeError("wrong object"),
                      OverflowError("range"), RecursionError("depth")):
            with self.subTest(error=type(error).__name__), \
                    mock.patch.object(self.budget, "qualify_receipt", side_effect=error):
                self.assert_refused()

    def test_receipt_loader_bounds_and_ambiguous_json_refuse(self):
        path = self.root / "receipt.json"
        for payload in (b'{', b'{"schema":1,"schema":2}', b'{"x":NaN}', b'\xff'):
            path.write_bytes(payload)
            with self.subTest(payload=payload), self.assertRaises(tool.InputError):
                tool.load_receipt(path)
        with mock.patch.object(self.budget, "MAX_INDEX_BYTES", 4):
            path.write_bytes(b'{} \n')
            self.assertEqual(tool.load_receipt(path), {})
            path.write_bytes(b'{}  \n')
            with self.assertRaisesRegex(tool.InputError, "oversized"):
                tool.load_receipt(path)


if __name__ == "__main__":
    unittest.main()
