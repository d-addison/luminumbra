#!/usr/bin/env python3
"""Adversarial file-only coverage/approval checks; no native qualification."""

from __future__ import annotations

import base64
import copy
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

import validate_visual_catalog as check


class CatalogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.catalog = check.read_json(check.ROOT / check.CATALOG)
        cls.workspace = tempfile.TemporaryDirectory(prefix="visual-catalog-contract-")
        cls.root = Path(cls.workspace.name)
        paths = {check.CATALOG, check.ORIGINAL_CONTRACT,
                 "src/luminumbra_client/core/RuntimeScenarioConfig.h"}
        for row in cls.catalog["rows"]:
            paths.update(row["fixture"]["source_paths"])
        paths.update(p.relative_to(check.ROOT).as_posix()
                     for p in (check.ROOT / check.EVIDENCE_DIR).iterdir() if p.is_file())
        for relative in paths:
            target = cls.root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(check.ROOT / relative, target)

    @classmethod
    def tearDownClass(cls):
        cls.workspace.cleanup()

    def setUp(self):
        self.data = copy.deepcopy(self.catalog)

    def row(self, scenario_id):
        return next(row for row in self.data["rows"] if row["id"] == scenario_id)

    def rejected(self, fragment):
        errors = check.validate(self.data, self.root)
        self.assertTrue(errors, "the invalid catalog passed")
        self.assertIn(fragment, "\n".join(errors))

    def test_restored_catalog_and_baselines_validate(self):
        self.assertEqual([], check.validate(self.data, self.root))
        self.assertEqual(82, len(self.data["rows"]))
        self.assertEqual(96, len(self.row("R01")["required_variants"]))

    def test_omitted_group_cannot_shrink_denominator(self):
        self.data["rows"].remove(self.row("U10"))
        self.rejected("roster changed")

    def test_original_cannot_be_reclassified_as_added(self):
        self.row("D22")["origin"] = "added"
        self.rejected("roster changed")

    def test_duplicate_id_is_not_another_group(self):
        self.data["rows"].append(copy.deepcopy(self.row("G04")))
        self.rejected("duplicate")

    def test_added_production_deliverable_cannot_disappear(self):
        self.data["rows"].remove(self.row("G10"))
        self.rejected("required added")

    def test_summer_only_cannot_satisfy_complete_sweep(self):
        self.row("R01")["required_variants"] = check.SWEEP[:48]
        self.rejected("original required_variants were reduced")

    def test_non_sweep_variants_cannot_be_removed(self):
        self.row("U03")["required_variants"].remove("future")
        self.rejected("original required_variants were reduced")

    def test_stills_cannot_replace_motion_requirement(self):
        self.row("G03")["temporal_evidence_required"] = False
        self.rejected("temporal requirement was removed")

    def test_added_scenarios_must_map_to_originals(self):
        self.row("B09")["parents"] = ["B10"]
        self.rejected("parent must be an original")

    def test_missing_capture_cannot_claim_a_command(self):
        self.row("B09")["capture"]["command"] = ["invented-renderer"]
        self.rejected("command must agree")

    def test_original_baseline_cannot_be_reassigned_to_current_source(self):
        self.row("R14")["evidence"][0]["source_commit"] = self.data["source_audit_commit"]
        self.rejected("preserve original deficient baseline")

    def test_existing_image_hash_is_checked(self):
        self.row("G04")["evidence"][0]["artifacts"][0]["sha256"] = "0" * 64
        self.rejected("hash mismatch")

    def test_baseline_pixels_remain_immutable_even_if_new_hash_is_supplied(self):
        path = self.root / check.EVIDENCE_DIR / "forest-reference.png"
        previous = path.read_bytes()
        try:
            path.write_bytes(previous + b"changed")
            self.row("G04")["evidence"][0]["artifacts"][0]["sha256"] = check.digest(path.read_bytes())
            self.rejected("historical G04 artifact identity changed")
        finally:
            path.write_bytes(previous)

    def test_source_discovery_catches_unmapped_runtime_selector(self):
        path = self.root / "src/luminumbra_client/core/RuntimeScenarioConfig.h"
        previous = path.read_text()
        try:
            path.write_text(previous + '\nbool new_capture() { return scenario == "new_capture"; }\n')
            self.rejected("selector discovery")
        finally:
            path.write_text(previous)

    def test_source_discovery_catches_new_ui_page(self):
        path = self.root / "data/ui/new_page.rml"
        try:
            path.write_text("<rml/>")
            self.rejected("UI documents")
        finally:
            path.unlink(missing_ok=True)

    def test_private_or_escaping_source_paths_are_rejected(self):
        for path in ("../private.json", "/tmp/private.json", "build/run/receipt.json", "."):
            with self.subTest(path=path):
                self.row("B09")["fixture"]["source_paths"] = [path]
                self.rejected("path")

    def test_symlink_does_not_publish_external_evidence(self):
        with tempfile.TemporaryDirectory() as outside:
            external = Path(outside) / "source.json"
            external.write_text("{}")
            link = self.root / "linked.json"
            try:
                link.symlink_to(external)
            except OSError:
                self.skipTest("symlink creation unavailable")
            try:
                self.row("B09")["fixture"]["source_paths"] = ["linked.json"]
                self.rejected("linked evidence")
            finally:
                link.unlink()

    def packet(self, scenario_id="R12"):
        """Synthetic contract data only; deliberately never native acceptance."""
        self.packet_scenario = scenario_id
        row = self.row(scenario_id)
        if row["fixture"]["status"] == "missing":
            row["fixture"]["status"] = "source_fixture"
            row["fixture"]["source_paths"] = self.row("R12")["fixture"]["source_paths"][:]
        packet_root = self.root / "docs/assets/contract-fixture"
        packet_root.mkdir(parents=True, exist_ok=True)
        payloads = {
            "original": base64.b64decode("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+j7WQAAAAASUVORK5CYII="),
            "functional": b'{"synthetic_contract_fixture":true}',
            "temporal": b'{"synthetic_contract_fixture":true}',
            "raw_performance": b'{"synthetic_contract_fixture":true}',
        }
        artifacts = []
        for role, content in payloads.items():
            file = packet_root / (role + (".png" if role == "original" else ".json"))
            file.write_bytes(content)
            artifacts.append({"id": role, "role": role, "path": file.relative_to(self.root).as_posix(),
                              "sha256": check.digest(content)})
        packet = {
            "schema": "luminumbra.visual_review_packet.v1", "scenario_id": "R12",
            "requirement_sha256": check.requirement_digest(row, self.data["policy"], self.root),
            "fixture_files": check.fixture_manifest(self.root, row),
            "source_commit": "a" * 40, "capture_status": "native_qualified", "correctness": "passed",
            "reproduction": {"command": "synthetic test only"},
            "hardware": dict.fromkeys(("os", "cpu", "gpu", "driver", "backend"), "synthetic")
                        | {"hardware_rendering": True},
            "camera_lighting": {"fixture": "synthetic"},
            "identities": dict.fromkeys(("engine_binary_sha256", "fixture_sha256", "asset_manifest_sha256",
                                          "tool_manifest_sha256"), "b" * 64),
            "performance": {"status": "measured", "target_status": "met", "scope": "synthetic"},
            "findings": {"scope": "synthetic"}, "aesthetic_review": {"scope": "synthetic"},
            "artifacts": artifacts,
            "variants": [],
        }
        packet["scenario_id"] = scenario_id
        packet["identities"]["fixture_sha256"] = check.manifest_digest(packet["fixture_files"])
        captures = []
        for index, variant in enumerate(row["required_variants"]):
            original = "original" if index == 0 else f"original-{index}"
            if index:
                self.artifact(packet, original, "original", payloads["original"], ".png")
            packet["variants"].append({"id": variant, "status": "passed", "originals": [original]})
            captures.append({"variant_id": variant, "run_id": "synthetic-run", "frame_id": index,
                             "original": original, "original_sha256": check.digest(payloads["original"])})
        manifest = self.identity(packet) | {"schema": "luminumbra.visual_capture_manifest.v1", "captures": captures}
        self.artifact(packet, "capture_manifest", "capture_manifest", manifest)
        packet["capture_manifest"] = "capture_manifest"
        row["execution_state"] = "ready_for_review"
        self.save_packet(packet)
        return packet

    def identity(self, packet):
        return {"scenario_id": packet["scenario_id"], "source_commit": packet["source_commit"],
                "engine_binary_sha256": packet["identities"]["engine_binary_sha256"],
                "fixture_sha256": packet["identities"]["fixture_sha256"]}

    def artifact(self, packet, artifact_id, role, value, suffix=".json"):
        path = self.root / "docs/assets/contract-fixture" / (artifact_id + suffix)
        content = value if isinstance(value, bytes) else json.dumps(value).encode()
        path.write_bytes(content)
        artifact = {"id": artifact_id, "role": role, "path": path.relative_to(self.root).as_posix(),
                    "sha256": check.digest(content)}
        packet["artifacts"] = [a for a in packet["artifacts"] if a["id"] != artifact_id] + [artifact]
        return artifact

    def read_artifact(self, packet, artifact_id):
        artifact = next(a for a in packet["artifacts"] if a["id"] == artifact_id)
        return check.read_json(self.root / artifact["path"])

    def measured(self, packet):
        requirements = self.row(self.packet_scenario)["performance_requirements"]
        performance = {"status": "measured", "scope": requirements["scope"], "target_status": "met", "profiles": []}
        for index in range(requirements["profile_count"]):
            profile_id = f"synthetic-profile-{index}"
            frozen = {"schema": "luminumbra.visual_performance_profile.v1", "profile_id": profile_id,
                      "scope": requirements["scope"], "configuration": {"synthetic_profile_index": index},
                      "statistics": {m["id"]: "mean" if m["statistic"] == "profile" else m["statistic"]
                                     for m in requirements["metrics"]}}
            manifest = self.artifact(packet, profile_id, "performance_profile", frozen)
            profile = {"id": profile_id, "manifest": profile_id, "metrics": []}
            for metric in requirements["metrics"]:
                value = metric["target"] + 1 if metric["operator"] == ">=" else metric["target"] / 2
                raw_id = profile_id + "-" + metric["id"]
                raw = self.identity(packet) | {"schema": "luminumbra.visual_performance_samples.v1",
                    "profile_id": profile_id, "profile_sha256": manifest["sha256"], "metric_id": metric["id"],
                    "samples": [{"value": value, "run_id": "synthetic-run", "frame_id": i} for i in range(100)]}
                self.artifact(packet, raw_id, "raw_performance", raw)
                profile["metrics"].append({"id": metric["id"], "raw_samples": raw_id, "value": value}
                                          | ({"reported_target": metric["reported_target"]}
                                             if "reported_target" in metric else {}))
            performance["profiles"].append(profile)
        packet["performance"] = performance
        self.save_packet(packet)

    def save_packet(self, packet):
        path = self.root / "docs/assets/contract-fixture/packet.json"
        path.write_text(json.dumps(packet))
        self.row(self.packet_scenario)["review_packet"] = {"path": path.relative_to(self.root).as_posix(),
                                              "sha256": check.digest(path.read_bytes())}

    def approve(self):
        row = self.row(self.packet_scenario)
        row["execution_state"] = "approved"
        row["approval"] = {"state": "approved", "actor": "user",
                           "packet_sha256": row["review_packet"]["sha256"],
                           "decision_reference": "synthetic test decision; never a real approval"}

    def test_consistent_synthetic_packet_does_not_automatically_approve(self):
        self.packet()
        self.assertEqual([], check.validate(self.data, self.root))
        self.assertEqual("pending", self.row("R12")["approval"]["state"])

    def test_silence_or_tests_cannot_create_approval(self):
        self.packet()
        self.approve()
        self.row("R12")["approval"]["actor"] = "tests"
        self.rejected("explicit user decision")

    def test_changed_packet_invalidates_previous_decision(self):
        packet = self.packet()
        self.approve()
        packet["findings"] = {"updated": "new result"}
        self.save_packet(packet)
        self.rejected("stale packet")

    def test_user_rejection_can_preserve_failed_diagnostic_packet(self):
        packet = self.packet()
        packet["correctness"] = "failed"
        packet["capture_status"] = "incomplete"
        packet["variants"][0]["status"] = "failed"
        self.save_packet(packet)
        self.approve()
        self.row("R12")["execution_state"] = "changes_requested"
        self.row("R12")["approval"]["state"] = "changes_requested"
        self.assertEqual([], check.validate(self.data, self.root))
        self.row("R12")["execution_state"] = "approved"
        self.row("R12")["approval"]["state"] = "approved"
        self.rejected("failed correctness")

    def test_changed_requirements_invalidate_review_packet(self):
        self.packet()
        self.row("R12")["required_variants"].append("new-shadow-control")
        self.rejected("does not cover current")

    def test_missing_or_failed_variant_cannot_be_review_ready(self):
        packet = self.packet()
        packet["variants"][0]["status"] = "missing"
        self.save_packet(packet)
        self.rejected("missing/failed variant")

    def test_preview_cannot_substitute_for_original(self):
        packet = self.packet()
        packet["artifacts"][0]["role"] = "preview"
        self.save_packet(packet)
        self.rejected("absent or is a preview")

    def test_missing_temporal_samples_rejects_still_only_packet(self):
        packet = self.packet()
        packet["artifacts"] = [a for a in packet["artifacts"] if a["role"] != "temporal"]
        self.save_packet(packet)
        self.rejected("automated temporal evidence")

    def test_missing_raw_performance_samples_rejects_measurement_claim(self):
        packet = self.packet()
        packet["artifacts"] = [a for a in packet["artifacts"] if a["role"] != "raw_performance"]
        self.save_packet(packet)
        self.rejected("raw samples")

    def test_software_rendering_cannot_claim_native_qualification(self):
        packet = self.packet()
        packet["hardware"]["hardware_rendering"] = False
        self.save_packet(packet)
        self.rejected("software rendering")

    def test_duplicate_json_keys_are_rejected(self):
        file = self.root / "duplicate.json"
        file.write_text('{"approval":"pending","approval":"approved"}')
        with self.assertRaisesRegex(ValueError, "duplicate JSON key"):
            check.read_json(file)

    def test_changed_fixture_bytes_invalidate_approval_at_same_path(self):
        self.packet()
        self.approve()
        path = self.root / self.row("R12")["fixture"]["source_paths"][0]
        previous = path.read_bytes()
        try:
            path.write_bytes(previous + b"\n// changed fixture\n")
            self.rejected("does not cover current fixture")
        finally:
            path.write_bytes(previous)

    def test_stale_fixture_manifest_cannot_follow_updated_requirement_digest(self):
        packet = self.packet()
        packet["fixture_files"] = {}
        self.save_packet(packet)
        self.rejected("fixture content manifest is stale")

    def test_capture_receipt_cannot_be_relabelled_to_another_source(self):
        packet = self.packet()
        manifest = self.read_artifact(packet, "capture_manifest")
        manifest["source_commit"] = "c" * 40
        self.artifact(packet, "capture_manifest", "capture_manifest", manifest)
        self.save_packet(packet)
        self.rejected("identity mismatch")

    def test_one_original_cannot_fill_all_96_sweep_cells(self):
        packet = self.packet("R01")
        manifest = self.read_artifact(packet, "capture_manifest")
        for variant in packet["variants"]:
            variant["originals"] = ["original"]
        for capture in manifest["captures"]:
            capture["original"] = "original"
        self.artifact(packet, "capture_manifest", "capture_manifest", manifest)
        self.save_packet(packet)
        self.rejected("original file reused across distinct variants")

    def test_copied_pixels_cannot_relabel_the_same_source_frame(self):
        packet = self.packet("R01")
        manifest = self.read_artifact(packet, "capture_manifest")
        manifest["captures"][1]["frame_id"] = 0
        self.artifact(packet, "capture_manifest", "capture_manifest", manifest)
        self.save_packet(packet)
        self.rejected("source frame reused across distinct variants")

    def test_independent_captures_may_have_identical_deterministic_pixels(self):
        self.packet("R01")
        self.assertEqual([], check.validate(self.data, self.root))

    def test_variant_capture_join_cannot_omit_a_cell(self):
        packet = self.packet()
        manifest = self.read_artifact(packet, "capture_manifest")
        manifest["captures"].pop()
        self.artifact(packet, "capture_manifest", "capture_manifest", manifest)
        self.save_packet(packet)
        self.rejected("join every variant original exactly")

    def test_accepted_scope_requirements_cannot_be_dropped(self):
        self.row("R24")["performance_requirements"] = check.REPORT_ONLY
        self.rejected("accepted scoped performance requirements changed")

    def test_accepted_world_workload_cannot_claim_performance_not_applicable(self):
        packet = self.packet("R24")
        packet["performance"] = {"status": "not_applicable", "rationale": "synthetic skip"}
        self.save_packet(packet)
        self.rejected("required performance cannot be N/A")

    def test_established_world_target_cannot_claim_not_established(self):
        packet = self.packet("R24")
        self.measured(packet)
        packet["performance"]["target_status"] = "not_established"
        self.save_packet(packet)
        self.rejected("established targets must be met")

    def test_both_distinct_world_profiles_are_required(self):
        packet = self.packet("R24")
        self.measured(packet)
        packet["performance"]["profiles"].pop()
        self.save_packet(packet)
        self.rejected("profiles are incomplete")

    def test_duplicate_world_configuration_cannot_count_as_second_profile(self):
        packet = self.packet("R24")
        self.measured(packet)
        profile = self.read_artifact(packet, "synthetic-profile-1")
        profile["configuration"] = {"synthetic_profile_index": 0}
        self.artifact(packet, "synthetic-profile-1", "performance_profile", profile)
        self.save_packet(packet)
        self.rejected("distinct workload configurations")

    def test_all_accepted_scopes_accept_complete_consistent_measurements(self):
        for scenario_id in ("R24", "R14", "B09"):
            with self.subTest(scenario_id=scenario_id):
                self.setUp()
                packet = self.packet(scenario_id)
                self.measured(packet)
                self.assertEqual([], check.validate(self.data, self.root))

    def test_claimed_metric_value_must_match_raw_samples(self):
        packet = self.packet("R24")
        self.measured(packet)
        packet["performance"]["profiles"][0]["metrics"][0]["value"] = 0
        self.save_packet(packet)
        self.rejected("differs from raw samples")

    def test_finite_samples_and_actual_frame_attribution_are_required(self):
        for field, value, message in (("value", float("nan"), "must be finite"),
                                      ("frame_id", None, "run/frame identity")):
            with self.subTest(field=field):
                self.setUp()
                packet = self.packet("R24")
                self.measured(packet)
                raw_id = "synthetic-profile-0-frame_ms"
                raw = self.read_artifact(packet, raw_id)
                raw["samples"][0][field] = value
                self.artifact(packet, raw_id, "raw_performance", raw)
                self.save_packet(packet)
                self.rejected(message)

    def test_raw_metric_cannot_be_attributed_to_different_profile(self):
        packet = self.packet("R24")
        self.measured(packet)
        raw_id = "synthetic-profile-0-frame_ms"
        raw = self.read_artifact(packet, raw_id)
        raw["profile_id"] = "synthetic-profile-1"
        self.artifact(packet, raw_id, "raw_performance", raw)
        self.save_packet(packet)
        self.rejected("profile/metric identity mismatch")

    def test_missed_p99_target_cannot_be_hidden_by_low_median(self):
        packet = self.packet("R24")
        self.measured(packet)
        raw_id = "synthetic-profile-0-frame_ms"
        raw = self.read_artifact(packet, raw_id)
        for sample in raw["samples"][-2:]:
            sample["value"] = 20
        self.artifact(packet, raw_id, "raw_performance", raw)
        packet["performance"]["profiles"][0]["metrics"][0]["value"] = 20
        self.save_packet(packet)
        self.rejected("required metric target missed")

    def test_strict_authoring_latency_bound_cannot_be_relaxed_to_equality(self):
        packet = self.packet("B09")
        self.measured(packet)
        raw_id = "synthetic-profile-0-camera_transform_ms"
        raw = self.read_artifact(packet, raw_id)
        for sample in raw["samples"]:
            sample["value"] = 100
        self.artifact(packet, raw_id, "raw_performance", raw)
        packet["performance"]["profiles"][0]["metrics"][1]["value"] = 100
        self.save_packet(packet)
        self.rejected("required metric target missed")

    def test_completion_mode_refuses_all_unfinished_groups(self):
        result = subprocess.run([sys.executable, str(check.ROOT / "tools/ci/validate_visual_catalog.py"),
                                 "--require-complete"], text=True, capture_output=True, check=False)
        self.assertEqual(1, result.returncode)
        self.assertIn("unfinished groups: R01", result.stderr)
        self.assertIn("G10", result.stderr)


if __name__ == "__main__":
    unittest.main()
