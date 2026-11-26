#!/usr/bin/env python3
"""Small synthetic recovery receipts; no engine execution or native evidence."""

from __future__ import annotations

import contextlib
import copy
import hashlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import recovery_durability_evidence as evidence

CANDIDATE = "a" * 40
BUILDS = {name: str(index + 1) * 64 for index, name in enumerate(evidence.LANES)}


def encoded(value):
    return json.dumps(value, sort_keys=True).encode("utf-8")


def receipt(files):
    records = [{"path": name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
               for name, data in sorted(files.items())]
    fold = hashlib.sha256()
    for item in records:
        fold.update(f"{item['path']}\0{item['size']}\0{item['sha256']}\n".encode("utf-8"))
    return {"schema": "luminumbra.save_receipt.v1", "file_count": len(records),
            "total_bytes": sum(item["size"] for item in records), "tree_digest": fold.hexdigest(),
            "files": records}


def fixture(root):
    documents = {}

    def artifact(name, value):
        data = value if type(value) is bytes else encoded(value)
        (root / name).write_bytes(data)
        documents[name] = data
        return {"path": name, "sha256": hashlib.sha256(data).hexdigest()}

    manifest = {"schema": evidence.SCHEMA, "candidate": CANDIDATE, "provenance": "synthetic", "lanes": []}
    for lane_name in evidence.LANES:
        lane = {"lane": lane_name, "candidate": CANDIDATE, "binary_sha256": BUILDS[lane_name],
                "compiler": "recorded compiler version", "configuration": "Release",
                "environment": {"os": lane_name, "filesystem": "recorded filesystem",
                                "storage": "virtual disk + controller + backing volume",
                                "cache": "guest and host cache policy", "virtualization": "recorded VM version"},
                "runs": []}
        tests = []
        junit = []
        for case in evidence.CASES:
            for mechanism in evidence.MECHANISMS:
                run_id = f"{case}-{mechanism}"
                basename = f"{lane_name}-{run_id}"
                testcase = f"RecoveryQualification.{run_id}"
                tests.append({"name": testcase, "command": ["recovery-tests", "--case", case, "--mechanism", mechanism]})
                junit.append(f'<testcase name="{testcase}" status="run"/>')
                identity = {"candidate": CANDIDATE, "lane": lane_name, "binary_sha256": BUILDS[lane_name],
                            "run_id": run_id, "case": case, "mechanism": mechanism}
                context = {key: lane[key] for key in ("lane", "candidate", "binary_sha256", "compiler",
                                                      "configuration", "environment")}
                raw_context = json.dumps(context, sort_keys=True, ensure_ascii=True, separators=(",", ":")).encode("utf-8")
                identity["context_sha256"] = hashlib.sha256(raw_context).hexdigest()
                run = {"id": run_id, "case": case, "mechanism": mechanism, "testcase": testcase, "exit_code": 0}
                cleanup = case.startswith("cleanup_")
                files = {"unrelated.bin": b"preserved"}
                if case in ("matching_restore", "abort_previous_commit") or cleanup:
                    files.update({"snapshot.index": b"index", "payload.bin": b"payload"})
                final_files = dict(files)
                if cleanup:
                    files["recovery/journal"] = b"owned evidence"
                for phase in ("before", "interrupted", "after", "expected"):
                    captured = files if phase in ("before", "interrupted") else final_files
                    if phase == "interrupted":
                        captured = {**captured, "recovery/journal": b"owned evidence"}
                    tree = receipt(captured)
                    run[phase] = artifact(f"{basename}-{phase}.json",
                                          {"schema": "luminumbra.recovery_member_tree.v1", "identity": identity,
                                           "phase": phase, "tree": tree})
                prefix = {"matching_restore": "matching_index_observed", "absent_create": "absent_index_observed",
                          "abort_previous_commit": "previous_commit_abort_started",
                          "abort_initial_creation": "initial_creation_abort_started",
                          "cleanup_committed_journal": "committed_journal_cleanup_started",
                          "cleanup_directory_retirement": "directory_retirement_started"}[case]
                boundary = "cleanup_completion" if cleanup else "index_completion"
                interruption = {"injected_fault": "fault_injected", "process_termination": "process_terminated",
                                "abrupt_vm_reset": "vm_reset_observed"}[mechanism]
                if mechanism == "injected_fault":
                    events = [prefix, interruption, boundary + "_failed", "recovery_retained"]
                else:
                    events = [prefix, boundary + "_pending", interruption, "recovery_retained"]
                events += ["retry_started"]
                events += ["cleanup_complete"] if cleanup else ["index_complete", "payload_started", "payload_complete"]
                trace = {"schema": "luminumbra.recovery_trace.v1", "identity": identity,
                         "events": events + ["recovery_retired"], "index_path": "snapshot.index",
                         "payload_path": "payload.bin", "recovery_paths": ["recovery/journal"]}
                run["trace"] = artifact(f"{basename}-trace.json", trace)
                if mechanism != "injected_fault":
                    run["interruption"] = artifact(f"{basename}-interruption.json", {
                        "schema": "luminumbra.recovery_interruption.v1", "identity": identity,
                        "method": mechanism, "observed": True, "controller": "external controller receipt",
                        "controller_log": artifact(f"{basename}-controller.txt", b"external observation\r\n"),
                        "graceful_shutdown": False, "guest_boot_before": "boot-one",
                        "guest_boot_after": "boot-two" if mechanism == "abrupt_vm_reset" else "boot-one",
                        "virtual_storage_stack": lane["environment"]["storage"]})
                lane["runs"].append(run)
        lane["discovery"] = artifact(f"{lane_name}-discovery.json",
                                     {"kind": "ctestInfo", "version": {"major": 1, "minor": 0}, "tests": tests})
        lane["junit"] = artifact(f"{lane_name}-junit.xml", ("<testsuite>" + "".join(junit) + "</testsuite>").encode())
        manifest["lanes"].append(lane)
    documents["recovery-evidence.json"] = encoded(manifest)
    (root / "recovery-evidence.json").write_bytes(documents["recovery-evidence.json"])
    return manifest, documents


class RecoveryDurabilityEvidenceContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temporary.name)
        cls.complete, cls.documents = fixture(cls.root)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def setUp(self):
        self.manifest = copy.deepcopy(self.complete)
        # Most refusal tests need only one cell; the full matrix runs twice in
        # the determinism regression. This keeps loaded Windows runners fast.
        self.manifest["lanes"] = self.manifest["lanes"][:1]
        self.lane = self.manifest["lanes"][0]
        self.lane["runs"] = self.lane["runs"][:1]
        self.run = self.lane["runs"][0]

    def write(self, name, data):
        path = self.root / name
        original = path.read_bytes()
        self.addCleanup(path.write_bytes, original)
        path.write_bytes(data)

    def mutate(self, ref, change):
        document = json.loads(self.documents[ref["path"]])
        change(document)
        data = encoded(document)
        self.write(ref["path"], data)
        ref["sha256"] = hashlib.sha256(data).hexdigest()

    def evaluate(self):
        self.write("recovery-evidence.json", encoded(self.manifest))
        return evidence.evaluate(self.root, CANDIDATE, BUILDS)

    def first(self):
        return self.evaluate()["checks"][0]

    def test_missing_lane_is_unevaluated(self):
        result = self.evaluate()
        self.assertEqual(result["status"], "incomplete")
        missing = [row for row in result["checks"] if row["lane"] == "posix"]
        self.assertTrue(missing)
        self.assertTrue(all(row["status"] == "unevaluated" and row["reason"] == "missing lane" for row in missing))

    def test_stale_candidate_refuses(self):
        self.manifest["candidate"] = "b" * 40
        with self.assertRaisesRegex(evidence.Refusal, "stale candidate"):
            self.evaluate()

    def test_process_kill_does_not_establish_power_loss(self):
        self.manifest = copy.deepcopy(self.complete)
        for lane in self.manifest["lanes"]:
            lane["runs"] = [run for run in lane["runs"] if run["mechanism"] != "abrupt_vm_reset"]
        result = self.evaluate()
        self.assertEqual(result["status"], "incomplete")
        self.assertFalse(result["native_qualified"])
        self.assertFalse(result["physical_power_loss_qualified"])
        self.assertTrue(all(row["status"] == "unevaluated" for row in result["checks"]
                            if row["mechanism"] == "abrupt_vm_reset"))

    def test_complete_synthetic_matrix_is_deterministic(self):
        self.manifest = copy.deepcopy(self.complete)
        first = self.evaluate()
        self.assertEqual(encoded(first), encoded(self.evaluate()))
        self.manifest["lanes"].reverse()
        for lane in self.manifest["lanes"]:
            lane["runs"].reverse()
        second = self.evaluate()
        # The input manifest digest changes with its bytes; the evaluated
        # checks, identities, scopes and all other artifact digests stay fixed.
        first["artifacts"].pop("recovery-evidence.json")
        second["artifacts"].pop("recovery-evidence.json")
        self.assertEqual(encoded(first), encoded(second))
        self.assertEqual(first["status"], "complete")
        self.assertEqual(len(first["checks"]), 54)
        self.assertFalse(first["native_qualified"])

    def test_native_label_is_only_a_declaration(self):
        self.manifest = copy.deepcopy(self.complete)
        self.assertFalse(self.evaluate()["native_declared"])
        self.manifest["provenance"] = "native"
        result = self.evaluate()
        self.assertEqual(result["status"], "complete")
        self.assertTrue(result["native_declared"])
        self.assertFalse(result["native_qualified"])

    def test_stale_build_lane_and_artifacts_are_incomplete(self):
        for target in ("candidate", "binary_sha256"):
            with self.subTest(target=target):
                original = self.lane[target]
                self.lane[target] = "b" * len(original)
                self.assertIn("stale", self.first()["reason"])
                self.lane[target] = original
        self.mutate(self.run["trace"], lambda doc: doc["identity"].update(run_id="other"))
        self.assertIn("identity", self.first()["reason"])

    def test_missing_cells_and_empty_evidence_stay_pending(self):
        self.lane["runs"] = []
        self.assertIn("missing matrix cell", self.first()["reason"])
        self.manifest["lanes"] = []
        self.assertTrue(all(row["status"] == "unevaluated" for row in self.evaluate()["checks"]))

    def test_unknown_and_duplicate_metadata_refuse(self):
        for field, value in (("schema", "unknown"), ("provenance", "unknown")):
            original = self.manifest[field]
            self.manifest[field] = value
            with self.assertRaises(evidence.Refusal):
                self.evaluate()
            self.manifest[field] = original
        self.manifest["lanes"].append(copy.deepcopy(self.lane))
        with self.assertRaisesRegex(evidence.Refusal, "duplicate lane"):
            self.evaluate()

    def test_duplicate_and_unknown_cells_cannot_count(self):
        self.lane["runs"].append(copy.deepcopy(self.run))
        self.assertIn("duplicate", self.first()["reason"])
        self.lane["runs"].pop()
        self.run["case"] = "unknown"
        self.assertIn("unknown", self.first()["reason"])

    def test_junit_failure_skip_disabled_and_duplicate_are_incomplete(self):
        ref = self.lane["junit"]
        original = self.documents[ref["path"]]
        for data in (original.replace(b"/>", b"><failure/></testcase>", 1),
                     original.replace(b"/>", b"><skipped/></testcase>", 1),
                     original.replace(b"status=\"run\"", b"status=\"notrun\"", 1),
                     original.replace(b"<testsuite>", b'<testsuite failures="1">'),
                     original.replace(b"</testsuite>", f'<testcase name="{self.run["testcase"]}" status="run"/></testsuite>'.encode()),
                     b"<testsuite/>", b"<partial", b'<!DOCTYPE x [<!ENTITY x "bad">]><testsuite/>'):
            with self.subTest(data=data[:40]):
                self.write(ref["path"], data)
                ref["sha256"] = hashlib.sha256(data).hexdigest()
                self.assertEqual(self.first()["status"], "unevaluated")

    def test_raw_discovery_and_execution_are_both_required(self):
        self.mutate(self.lane["discovery"], lambda doc: doc.update(tests=[]))
        self.assertIn("discovery", self.first()["reason"])
        self.lane["discovery"] = copy.deepcopy(self.complete["lanes"][0]["discovery"])
        self.write(self.lane["discovery"]["path"], self.documents[self.lane["discovery"]["path"]])
        self.run["exit_code"] = True
        self.assertIn("executed test", self.first()["reason"])

    def test_discovery_refuses_unknown_versions_duplicates_and_missing_commands(self):
        for change in (lambda doc: doc.update(version={"major": True, "minor": 0}),
                       lambda doc: doc.update(kind="unknown"),
                       lambda doc: doc["tests"].append(doc["tests"][0]),
                       lambda doc: doc["tests"][0].update(command=[]),
                       lambda doc: doc["tests"].pop(0)):
            self.mutate(self.lane["discovery"], change)
            self.assertEqual(self.first()["status"], "unevaluated")

    def test_payload_before_index_and_failed_retention_are_incomplete(self):
        ref = self.run["trace"]
        for change in (lambda doc: doc["events"].insert(1, "payload_started"),
                       lambda doc: doc["events"].remove("recovery_retained"),
                       lambda doc: doc["events"].remove("retry_started"),
                       lambda doc: doc["events"].insert(1, "unknown_event"),
                       lambda doc: doc.update(events=[])):
            self.mutate(ref, change)
            self.assertIn("trace", self.first()["reason"])

    def test_every_case_refuses_premature_retirement(self):
        for case in evidence.CASES:
            with self.subTest(case=case):
                run = next(copy.deepcopy(item) for item in self.complete["lanes"][0]["runs"]
                           if item["case"] == case and item["mechanism"] == "injected_fault")
                self.lane["runs"] = [run]
                self.mutate(run["trace"], lambda doc: doc["events"].insert(1, "recovery_retired"))
                row = next(item for item in self.evaluate()["checks"]
                           if item["case"] == case and item["mechanism"] == "injected_fault")
                self.assertIn("trace ordering", row["reason"])

    def test_member_tree_counts_hashes_ranges_and_paths_are_checked(self):
        for change in (lambda doc: doc["tree"].update(file_count=True),
                       lambda doc: doc["tree"].update(total_bytes=1e38),
                       lambda doc: doc["tree"].update(tree_digest="0" * 64),
                       lambda doc: doc["tree"]["files"][0].update(path="../escape"),
                       lambda doc: doc["tree"]["files"][0].update(size=-1),
                       lambda doc: doc["tree"]["files"].reverse(),
                       lambda doc: doc.update(phase="unknown"),
                       lambda doc: doc.update(schema="unknown")):
            self.mutate(self.run["before"], change)
            self.assertEqual(self.first()["status"], "unevaluated")

    def test_after_tree_must_match_independent_expected_tree(self):
        self.mutate(self.run["after"], lambda doc: doc.update(tree=receipt({"other.bin": b"wrong"})))
        self.assertIn("expected tree", self.first()["reason"])

    def test_interrupted_tree_proves_retention_matching_and_absence(self):
        for position, interrupted, reason in (
                (0, {"payload.bin": b"payload", "snapshot.index": b"index", "unrelated.bin": b"preserved"},
                 "recovery evidence missing"),
                (0, {"payload.bin": b"wrong", "snapshot.index": b"index", "unrelated.bin": b"preserved",
                     "recovery/journal": b"owned evidence"}, "matching member bytes"),
                (3, {"snapshot.index": b"foreign", "unrelated.bin": b"preserved",
                     "recovery/journal": b"owned evidence"}, "absent creates")):
            with self.subTest(position=position, reason=reason):
                run = copy.deepcopy(self.complete["lanes"][0]["runs"][position])
                self.lane["runs"] = [run]
                self.mutate(run["interrupted"], lambda doc: doc.update(tree=receipt(interrupted)))
                row = next(item for item in self.evaluate()["checks"]
                           if item["case"] == run["case"] and item["mechanism"] == run["mechanism"])
                self.assertIn(reason, row["reason"])

    def test_cleanup_inventory_must_be_exact_and_nonempty(self):
        run = copy.deepcopy(self.complete["lanes"][0]["runs"][-3])
        self.lane["runs"] = [run]
        for names in ([], ["recovery/journal", "recovery/journal"], ["other"], ["../escape"], ["payload.bin"]):
            self.mutate(run["trace"], lambda doc: doc.update(recovery_paths=names))
            row = next(item for item in self.evaluate()["checks"]
                       if item["case"] == run["case"] and item["mechanism"] == run["mechanism"])
            self.assertEqual(row["status"], "unevaluated")

    def test_cleanup_cannot_retire_committed_members(self):
        self.lane["runs"] = [copy.deepcopy(self.complete["lanes"][0]["runs"][-3])]
        run = self.lane["runs"][0]
        for phase in ("after", "expected"):
            self.mutate(run[phase], lambda doc: doc.update(tree=receipt({"unrelated.bin": b"preserved"})))
        rows = self.evaluate()["checks"]
        row = next(item for item in rows if item["case"] == run["case"] and item["mechanism"] == run["mechanism"])
        self.assertEqual(row["status"], "unevaluated")
        self.assertIn("committed members", row["reason"])

    def test_vm_reset_requires_external_observation_and_storage_scope(self):
        self.lane["runs"] = [copy.deepcopy(self.complete["lanes"][0]["runs"][2])]
        run = self.lane["runs"][0]
        for change in (lambda doc: doc.update(method="process_termination"),
                       lambda doc: doc.update(observed=False),
                       lambda doc: doc.update(graceful_shutdown=True),
                       lambda doc: doc.update(guest_boot_after="boot-one"),
                       lambda doc: doc.update(virtual_storage_stack="other")):
            self.mutate(run["interruption"], change)
            row = self.evaluate()["checks"][2]
            self.assertEqual(row["status"], "unevaluated")

    def test_environment_fields_cannot_be_omitted(self):
        for key in tuple(self.lane["environment"]):
            original = self.lane["environment"].pop(key)
            self.assertEqual(self.first()["status"], "unevaluated")
            self.lane["environment"][key] = original
        for value in ("unknown", "pending", "", None):
            self.lane["environment"]["storage"] = value
            self.assertEqual(self.first()["status"], "unevaluated")

    def test_relabeling_build_or_storage_configuration_is_stale(self):
        for field in ("compiler", "configuration"):
            original = self.lane[field]
            self.lane[field] = "different recorded setting"
            self.assertIn("identity", self.first()["reason"])
            self.lane[field] = original
        for field in self.lane["environment"]:
            original = self.lane["environment"][field]
            self.lane["environment"][field] = "different recorded setting"
            self.assertIn("identity", self.first()["reason"])
            self.lane["environment"][field] = original

    def test_process_interruption_and_controller_logs_must_be_observed(self):
        run = copy.deepcopy(self.complete["lanes"][0]["runs"][1])
        self.lane["runs"] = [run]
        for change in (lambda doc: doc.update(guest_boot_after="changed-boot"),
                       lambda doc: doc["controller_log"].update(path="missing-controller.log")):
            self.mutate(run["interruption"], change)
            self.assertEqual(self.evaluate()["checks"][1]["status"], "unevaluated")
        del run["interruption"]
        self.assertEqual(self.evaluate()["checks"][1]["status"], "unevaluated")

    def test_missing_unreadable_partial_and_digest_mismatch_refuse(self):
        ref = self.run["trace"]
        original = copy.deepcopy(ref)
        ref["path"] = "missing.json"
        self.assertEqual(self.first()["status"], "unevaluated")
        ref.update(original)
        self.write(ref["path"], b"partial")
        self.assertIn("digest mismatch", self.first()["reason"])
        ref["sha256"] = hashlib.sha256(b"partial").hexdigest()
        self.assertEqual(self.first()["status"], "unevaluated")
        with mock.patch.object(evidence, "read_bytes", side_effect=PermissionError("denied")):
            with self.assertRaises(PermissionError):
                evidence.evaluate(self.root, CANDIDATE, BUILDS)

    def test_path_escape_drive_names_and_source_tree_outputs_refuse(self):
        ref = self.run["trace"]
        for name in ("../outside", "C:/capture.json", "C:capture.json", "/absolute", "a\\b", "a/../b", ""):
            ref["path"] = name
            self.assertEqual(self.first()["status"], "unevaluated")
        with self.assertRaisesRegex(evidence.Refusal, "outside source"):
            evidence.evaluate(self.root, CANDIDATE, BUILDS, [self.root.parent])

    def test_nonregular_and_escaped_links_refuse_without_link_privilege(self):
        from fixture_paths import open_regular
        with self.assertRaises(ValueError):
            with open_regular(self.root, self.root):
                self.fail("directory opened")
        # The shared reader owns native link and Windows long-path checks.
        # Exercise its refusal at this consumer even when link creation is denied.
        with mock.patch.object(evidence, "resolve_reference", side_effect=ValueError("outside root")):
            with self.assertRaisesRegex(ValueError, "outside root"):
                evidence.evaluate(self.root, CANDIDATE, BUILDS)

    def test_parsing_budgets_and_duplicate_json_keys_refuse(self):
        with self.assertRaisesRegex(evidence.Refusal, "duplicate JSON"):
            evidence.parse_json(b'{"schema":"one","schema":"two"}')
        with self.assertRaises(evidence.Refusal):
            evidence.parse_json(b'{"value":NaN}')
        with self.assertRaises(evidence.Refusal):
            evidence.parse_json(b'{"value":1e999}')
        oversized = self.root / "oversized.json"
        self.addCleanup(oversized.unlink)
        oversized.write_bytes(b"x" * (evidence.MAX_ARTIFACT_BYTES + 1))
        with self.assertRaisesRegex(ValueError, "oversized"):
            evidence.Artifacts(self.root).read(oversized.name)
        with mock.patch.object(evidence, "MAX_TOTAL_BYTES", 1):
            with self.assertRaisesRegex(ValueError, "oversized fixture"):
                evidence.evaluate(self.root, CANDIDATE, BUILDS)
        with mock.patch.object(evidence, "MAX_ARTIFACTS", 0):
            with self.assertRaisesRegex(evidence.Refusal, "count budget"):
                evidence.evaluate(self.root, CANDIDATE, BUILDS)

    def test_empty_and_oversized_record_sets_are_not_success(self):
        for field, value in (("events", []), ("events", ["event"] * (evidence.MAX_EVENTS + 1)),
                             ("events", None), ("index_path", "payload.bin")):
            self.mutate(self.run["trace"], lambda doc: doc.update({field: value}))
            self.assertEqual(self.first()["status"], "unevaluated")
        with mock.patch.object(evidence, "MAX_RECORDS", 0):
            self.assertEqual(self.first()["status"], "unevaluated")
        with mock.patch.object(evidence, "MAX_TESTS", 0):
            self.assertEqual(self.first()["status"], "unevaluated")

    def test_failed_reads_consume_budget_and_empty_artifacts_refuse(self):
        with mock.patch.object(evidence, "MAX_TOTAL_BYTES", 6), \
                mock.patch.object(evidence, "MAX_ARTIFACT_BYTES", 4), \
                mock.patch.object(evidence, "read_bytes", side_effect=OSError("partial read")) as reader:
            artifacts = evidence.Artifacts(self.root)
            for _ in range(2):
                with self.assertRaises(OSError):
                    artifacts.read("recovery-evidence.json")
            with self.assertRaisesRegex(evidence.Refusal, "byte budget"):
                artifacts.read("recovery-evidence.json")
            self.assertEqual(reader.call_count, 2)
        ref = self.run["trace"]
        self.write(ref["path"], b"")
        ref["sha256"] = hashlib.sha256(b"").hexdigest()
        self.assertIn("empty artifact", self.first()["reason"])

    def test_junit_node_budget_refuses_during_construction(self):
        with mock.patch.object(evidence, "MAX_TESTS", 1):
            with self.assertRaisesRegex(evidence.Refusal, "node budget"):
                evidence.passed_tests(b"<testsuite>" + b"<testcase/>" * 8 + b"</testsuite>")

    def test_junit_encodings_cannot_bypass_dtd_refusal(self):
        for encoding in ("utf-8", "utf-16"):
            data = '<!DOCTYPE x [<!ENTITY x "bad">]><testsuite/>'
            with self.assertRaisesRegex(evidence.Refusal, "DTD"):
                evidence.passed_tests(data.encode(encoding))
            valid = ('<testsuite><testcase name="Recovery" status="run">'
                     '<system-out><![CDATA[literal <!DOCTYPE text]]></system-out>'
                     '</testcase></testsuite>')
            self.assertEqual(evidence.passed_tests(valid.encode(encoding)), {"Recovery"})

    def test_expected_builds_must_be_independent_complete_and_valid(self):
        for builds in ({}, {**BUILDS, "other": "f" * 64}, {**BUILDS, "posix": "bad"}):
            with self.assertRaises(evidence.Refusal):
                evidence.evaluate(self.root, CANDIDATE, builds)

    def test_cli_prints_incomplete_and_rejects_duplicate_builds(self):
        self.write("recovery-evidence.json", encoded(self.manifest))
        argv = [str(self.root), "--expected-candidate", CANDIDATE]
        for name, sha in BUILDS.items():
            argv.extend(["--build", f"{name}={sha}"])
        for extra in ([], ["--build", f"posix={BUILDS['posix']}"]):
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(evidence.main(argv + extra), 1)
            self.assertFalse(json.loads(output.getvalue())["native_qualified"])

    def test_crlf_discovery_and_junit_are_accepted(self):
        for ref in (self.lane["discovery"], self.lane["junit"]):
            data = self.documents[ref["path"]] + b"\r\n"
            self.write(ref["path"], data)
            ref["sha256"] = hashlib.sha256(data).hexdigest()
        self.assertEqual(self.first()["status"], "complete")


if __name__ == "__main__":
    unittest.main()
