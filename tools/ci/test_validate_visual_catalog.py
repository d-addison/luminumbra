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

    def packet(self):
        """Synthetic contract data only; deliberately never native acceptance."""
        row = self.row("R12")
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
            "requirement_sha256": check.requirement_digest(row, self.data["policy"]),
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
            "variants": [{"id": variant, "status": "passed", "originals": ["original"]}
                         for variant in row["required_variants"]],
        }
        row["execution_state"] = "ready_for_review"
        self.save_packet(packet)
        return packet

    def save_packet(self, packet):
        path = self.root / "docs/assets/contract-fixture/packet.json"
        path.write_text(json.dumps(packet))
        self.row("R12")["review_packet"] = {"path": path.relative_to(self.root).as_posix(),
                                              "sha256": check.digest(path.read_bytes())}

    def approve(self):
        row = self.row("R12")
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

    def test_completion_mode_refuses_all_unfinished_groups(self):
        result = subprocess.run([sys.executable, str(check.ROOT / "tools/ci/validate_visual_catalog.py"),
                                 "--require-complete"], text=True, capture_output=True, check=False)
        self.assertEqual(1, result.returncode)
        self.assertIn("unfinished groups: R01", result.stderr)
        self.assertIn("G10", result.stderr)


if __name__ == "__main__":
    unittest.main()
