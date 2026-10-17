#!/usr/bin/env python3
"""Refusal regressions for manual CI evidence; no native processes are launched."""
import json
from pathlib import Path, PurePosixPath, PureWindowsPath
import tempfile
import types
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET
import zipfile

import qualification_capture as capture

HEAD = "a" * 40
OTHER = "b" * 40
HASH = "1" * 64
RECEIPT = {"version": "1", "seed": "424242", "ticks": "64", "ceiling": "256",
           "wind_ctor": "01", "wind_evolved": "02", "weather_ctor": "03",
           "weather_evolved": "04", "aether_ctor": "05", "aether_evolved": "06"}
LINE = "AMBIENT_RECEIPT " + " ".join(f"{key}={value}" for key, value in RECEIPT.items())


def environment(lane="linux-gcc"):
    return {"GITHUB_REPOSITORY": capture.REPO, "GITHUB_EVENT_NAME": "workflow_dispatch",
            "GITHUB_SHA": HEAD, "QUALIFICATION_WORKFLOW_SHA": HEAD,
            "QUALIFICATION_WORKFLOW_REF": f"{capture.REPO}/.github/workflows/{capture.LANES[lane][0]}@refs/heads/candidate",
            "GITHUB_RUN_ID": "10", "GITHUB_RUN_ATTEMPT": "2"}


class CaptureContract(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def junit(self, producers=1, duplicate=False, failed=None, property_too=False, suite=False):
        root = ET.Element("testsuite")
        for _ in range(producers):
            case = ET.SubElement(root, "testcase", name="AmbientDeterminismReceipt.EmitsConstructorAndEvolvedHashes")
            ET.SubElement(case, "system-out").text = LINE + ("\n" + LINE if duplicate else "")
            if property_too:
                ET.SubElement(case, "properties")
                ET.SubElement(case.find("properties"), "property", name="ambient_receipt", value=LINE)
            if failed:
                ET.SubElement(case, failed)
        if suite:
            ET.SubElement(root, "system-out").text = LINE
        path = self.root / "ctest.junit.xml"
        ET.ElementTree(root).write(path)
        return path

    def comparator(self):
        return types.SimpleNamespace(_parse_receipt=lambda *_: RECEIPT.copy(), InputError=ValueError)

    def test_same_head_accepts_but_workflow_event_checkout_and_repo_must_agree(self):
        self.assertEqual(capture.identity("linux-gcc", HEAD, environment(), HEAD)["run_attempt"], 2)
        for key, bad in (("GITHUB_SHA", OTHER), ("QUALIFICATION_WORKFLOW_SHA", OTHER),
                         ("GITHUB_REPOSITORY", "foreign/repo"), ("GITHUB_EVENT_NAME", "pull_request"),
                         ("QUALIFICATION_WORKFLOW_REF", "foreign/repo/.github/workflows/ci.yml@refs/heads/candidate"),
                         ("GITHUB_RUN_ATTEMPT", "0")):
            with self.subTest(key=key), self.assertRaises(ValueError):
                capture.identity("linux-gcc", HEAD, {**environment(), key: bad}, HEAD)
        with self.assertRaises(ValueError):
            capture.identity("linux-gcc", HEAD, environment(), OTHER)
        with self.assertRaises(ValueError):
            capture.identity("linux-gcc", "aaaa", environment(), HEAD)

    def test_same_producer_property_and_stdout_are_one_receipt(self):
        with patch.object(capture, "module", return_value=self.comparator()):
            self.assertEqual(capture.ambient_receipt(self.junit(property_too=True)), RECEIPT)

    def test_exact_ctest_and_google_test_producer_identities(self):
        identities = (
            (None, "AmbientDeterminismReceipt.EmitsConstructorAndEvolvedHashes", True),
            ("AmbientDeterminismReceipt", "EmitsConstructorAndEvolvedHashes", True),
            (None, "Other.AmbientDeterminismReceipt.EmitsConstructorAndEvolvedHashes", False),
            ("Other", "EmitsConstructorAndEvolvedHashes", False),
            (None, "EmitsConstructorAndEvolvedHashes", False),
            (None, "AmbientDeterminismReceipt.Other", False),
            (None, "", False),
            (None, None, False),
        )
        for classname, name, accepted in identities:
            with self.subTest(classname=classname, name=name):
                path = self.junit(property_too=True)
                tree = ET.parse(path)
                case = tree.getroot().find("testcase")
                if classname is not None:
                    case.set("classname", classname)
                if name is None:
                    del case.attrib["name"]
                else:
                    case.set("name", name)
                tree.write(path)
                with patch.object(capture, "module", return_value=self.comparator()):
                    if accepted:
                        self.assertEqual(capture.ambient_receipt(path), RECEIPT)
                    else:
                        with self.assertRaisesRegex(ValueError, "unexpected testcase"):
                            capture.ambient_receipt(path)

    def test_unrelated_disabled_case_does_not_invalidate_attributed_receipt(self):
        path = self.junit(property_too=True)
        tree = ET.parse(path)
        ET.SubElement(tree.getroot(), "testcase", name="Unrelated", status="disabled")
        tree.write(path)
        with patch.object(capture, "module", return_value=self.comparator()):
            self.assertEqual(capture.ambient_receipt(path), RECEIPT)

    def test_zero_duplicate_failed_and_unattributed_receipts_refuse(self):
        for arguments in ({"producers": 0}, {"producers": 2}, {"duplicate": True},
                          {"failed": "failure"}, {"failed": "error"}, {"failed": "skipped"}, {"suite": True}):
            with self.subTest(arguments=arguments), patch.object(capture, "module", return_value=self.comparator()), self.assertRaises(ValueError):
                capture.ambient_receipt(self.junit(**arguments))

    def test_unevaluated_receipt_testcases_refuse(self):
        for field, value in (("status", "disabled"), ("status", "notrun"),
                             ("status", "skipped"), ("status", "unknown"), ("status", ""),
                             ("result", "suppressed"), ("result", "skipped"),
                             ("result", "unknown"), ("result", "")):
            with self.subTest(field=field, value=value):
                path = self.junit(property_too=True)
                tree = ET.parse(path)
                tree.getroot().find("testcase").set(field, value)
                tree.write(path)
                with patch.object(capture, "module", return_value=self.comparator()), self.assertRaisesRegex(ValueError, "unevaluated"):
                    capture.ambient_receipt(path)

    def test_explicit_success_status_and_result_are_accepted(self):
        path = self.junit()
        tree = ET.parse(path)
        tree.getroot().find("testcase").attrib.update(status="run", result="completed")
        tree.write(path)
        with patch.object(capture, "module", return_value=self.comparator()):
            self.assertEqual(capture.ambient_receipt(path), RECEIPT)

    def test_receipt_parser_error_and_changed_ceiling_refuse(self):
        parser = types.SimpleNamespace(_parse_receipt=lambda *_: {**RECEIPT, "ceiling": "1024"}, InputError=ValueError)
        with patch.object(capture, "module", return_value=parser), self.assertRaises(ValueError):
            capture.ambient_receipt(self.junit())
        parser._parse_receipt = lambda *_: (_ for _ in ()).throw(ValueError("malformed receipt"))
        with patch.object(capture, "module", return_value=parser), self.assertRaisesRegex(ValueError, "malformed receipt"):
            capture.ambient_receipt(self.junit())

    def test_masked_failure_skip_and_missing_producer_refuse(self):
        outcomes = dict.fromkeys(capture.LANES["linux-gcc"][2], "success")
        capture.successful_producers("linux-gcc", outcomes)
        for state in ("failure", "skipped", "cancelled", ""):
            with self.subTest(state=state), self.assertRaises(ValueError):
                capture.successful_producers("linux-gcc", {**outcomes, "tests": state})
        outcomes.pop("tests")
        with self.assertRaises(ValueError):
            capture.successful_producers("linux-gcc", outcomes)

    def document(self):
        return {**capture.identity("linux-gcc", HEAD, environment(), HEAD), "schema": capture.SCHEMA,
                "status": "complete", "native_qualified": False, "binary_availability": "not-exported", "source_dirty": False,
                "fixture_scope": "headless-server", "fixture_hash": HASH, "binary_sha256": HASH,
                "producers": dict.fromkeys(capture.LANES["linux-gcc"][2], "success"),
                "files": {"ctest.junit.xml": capture.digest(self.junit())}, "ambient": RECEIPT}

    def test_runner_loss_and_different_attempt_refuse_independent_job_proof(self):
        doc = self.document()
        run = {"head_sha": HEAD, "event": "workflow_dispatch", "run_attempt": 2, "path": doc["workflow_path"]}
        job = {"id": 1, "name": doc["job_name"], "status": "completed", "conclusion": "success", "completed_at": "2026-10-11T00:00:00Z"}
        for field, value in (("conclusion", "failure"), ("conclusion", "skipped"), ("status", "in_progress")):
            with self.subTest(field=field, value=value), patch.object(capture, "api", side_effect=[run, {"total_count": 1, "jobs": [{**job, field: value}]}]), self.assertRaises(ValueError):
                capture.verify_producer(doc)
        with patch.object(capture, "api", return_value={**run, "run_attempt": 1}), self.assertRaises(ValueError):
            capture.verify_producer(doc)
        with patch.object(capture, "api", side_effect=[run, {"total_count": 1, "jobs": [job]}]):
            self.assertEqual(capture.verify_producer(doc)["job_id"], 1)

    def test_paginated_jobs_reject_repeated_page(self):
        doc = self.document()
        run = {"head_sha": HEAD, "event": "workflow_dispatch", "run_attempt": 2, "path": doc["workflow_path"]}
        page = {"total_count": 200, "jobs": [{"id": i, "name": "other"} for i in range(100)]}
        with patch.object(capture, "api", side_effect=[run, page, page]), self.assertRaisesRegex(ValueError, "duplicate producer"):
            capture.verify_producer(doc)

    def archive(self, document):
        path = self.root / "capture.zip"
        with zipfile.ZipFile(path, "w") as zipped:
            zipped.writestr("qualification-capture.json", json.dumps(document))
            zipped.write(self.root / "ctest.junit.xml", "ctest.junit.xml")
        return path

    def artifact_row(self, path):
        return {"id": 99, "name": f"qualification-linux-gcc-{HEAD}-10-2", "expired": False,
                "workflow_run": {"id": 10, "head_sha": HEAD}, "digest": "sha256:" + capture.digest(path),
                "size_in_bytes": path.stat().st_size}

    def test_artifact_digest_attempt_expiry_and_origin_are_bound(self):
        doc = self.document()
        archive = self.archive(doc)
        row = self.artifact_row(archive)
        with patch.object(capture, "api", return_value={"total_count": 1, "artifacts": [row]}):
            self.assertEqual(capture.verify_archive("linux-gcc", 10, 2, archive, doc)["artifact_id"], 99)
        for mutation in ({"digest": "sha256:" + "0" * 64}, {"expired": True},
                         {"workflow_run": {"id": 10, "head_sha": OTHER}},
                         {"name": f"qualification-linux-gcc-{HEAD}-10-1"}):
            with self.subTest(mutation=mutation), patch.object(capture, "api", return_value={"total_count": 1, "artifacts": [{**row, **mutation}]}), self.assertRaises(ValueError):
                capture.verify_archive("linux-gcc", 10, 2, archive, doc)
        with self.assertRaises(ValueError):
            capture.verify_archive("linux-gcc", 10, 1, archive, doc)

    def test_corrupt_junit_is_rejected_before_github_proof(self):
        document = self.document()
        path = self.root / "qualification-capture.json"
        path.write_text(json.dumps(document), encoding="utf-8")
        self.root.joinpath("ctest.junit.xml").write_text("changed", encoding="utf-8")
        with patch.object(capture, "command", side_effect=[HEAD, ""]), patch.object(capture, "module", return_value=types.SimpleNamespace(fixture_hash=lambda *_args, **_kwargs: HASH)), patch.object(capture, "verify_producer") as proof, self.assertRaisesRegex(ValueError, "digest"):
            capture.validate_chain(HEAD, [path])
        proof.assert_not_called()

    def test_missing_lane_and_wrong_head_refuse(self):
        with self.assertRaises(ValueError):
            capture.validate_archives(HEAD, [])
        with patch.object(capture, "command", return_value=OTHER), self.assertRaisesRegex(ValueError, "same candidate"):
            capture.validate_chain(HEAD, [])

    def test_server_capture_wrong_revision_dirty_parameters_and_command_refuse(self):
        source = self.root.resolve()
        manifest = {"revision": HEAD, "source_dirty": False, "fixture_hash": HASH,
                    "binary_sha256": HASH, **capture.PARAMETERS, "runs": []}
        commands = {}
        for preset in capture.PRESETS:
            argv = [str(source / capture.SERVER_BINARY), "--root", str(source), "--smoke",
                    "--no-audio", "--sim-budget", "--ecology-roster", "--planted-roster",
                    "--preset", preset, "--seed", "1337", "--ticks", "600",
                    "--autosave-ticks", "30", "--radius", "1",
                    "--collision-radius", "1", "--artifact",
                    str(source / f"build/qualification/sim-budget-capture/{preset}.json")]
            commands[preset] = argv
            manifest["runs"].append({"preset": preset, "artifact": f"{preset}.json", "command": argv})
        modules = {"sim_budget_io": types.SimpleNamespace(load_capture=lambda _: {"manifest": manifest}),
                   "capture_sim_budget": types.SimpleNamespace(command=lambda *_args: commands[_args[3]])}
        with patch.object(capture, "module", side_effect=modules.__getitem__):
            capture.capture_contract(self.root, HEAD, HASH, HASH, str(source))
            for key, value in (("revision", OTHER), ("source_dirty", True), ("ticks", 601),
                               ("surface_radius", True), ("fixture_hash", "0" * 64)):
                original = manifest[key]
                manifest[key] = value
                with self.subTest(key=key), self.assertRaises(ValueError):
                    capture.capture_contract(self.root, HEAD, HASH, HASH, str(source))
                manifest[key] = original
            manifest["runs"][0]["command"] = [*commands["default"][:-1], "/foreign/default.json"]
            with self.assertRaisesRegex(ValueError, "command"):
                capture.capture_contract(self.root, HEAD, HASH, HASH, str(source))
            manifest["runs"] = manifest["runs"][:-1]
            with self.assertRaisesRegex(ValueError, "three ordered"):
                capture.capture_contract(self.root, HEAD, HASH, HASH, str(source))

    def test_duplicate_json_keys_are_not_ambiguous(self):
        path = self.root / "duplicate.json"
        path.write_text('{"run_attempt":1,"run_attempt":2}', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "duplicate JSON key"):
            capture.read_json(path)

    def test_server_capture_producer_path_syntax_is_independent_of_verifier_host(self):
        for source in (PurePosixPath("/producer/source"), PureWindowsPath("C:/producer/source")):
            manifest = {"revision": HEAD, "source_dirty": False, "fixture_hash": HASH,
                        "binary_sha256": HASH, **capture.PARAMETERS, "runs": []}
            for preset in capture.PRESETS:
                artifact = f"{preset}.json"
                argv = capture.module("capture_sim_budget").command(
                    source / capture.SERVER_BINARY, source,
                    source / "build/qualification/sim-budget-capture" / artifact,
                    preset, "1337", 600, 1, 1)
                manifest["runs"].append({"preset": preset, "artifact": artifact, "command": argv})
            loader = types.SimpleNamespace(load_capture=lambda _: {"manifest": manifest})
            command_module = capture.module("capture_sim_budget")
            modules = {"sim_budget_io": loader, "capture_sim_budget": command_module}
            with self.subTest(source=str(source)), patch.object(capture, "module", side_effect=modules.__getitem__):
                capture.capture_contract(self.root, HEAD, HASH, HASH, str(source))


if __name__ == "__main__":
    unittest.main()
