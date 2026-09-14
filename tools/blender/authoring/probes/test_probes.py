"""Contract and lifecycle checks; these do not qualify engine rendering."""
from __future__ import annotations

import copy
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from common import atomic_json, canonical, digest, parse_json, read_json
from compare_exports import compare
from contracts import Refusal, graph_semantic_digest, project_path, validate_asset
from fixtures import asset, cases, graph, run
from mock_service import MockService


class Contracts(unittest.TestCase):
    def test_valid_asset(self):
        validate_asset(asset())

    def test_refuse_unknown_required_schema(self):
        value = asset()
        value["required_schemas"] = ["game.unknown.v99"]
        with self.assertRaises(Refusal):
            validate_asset(value)

    def test_refuse_unknown_profile_and_fields(self):
        for key, value in (("profile", "production"), ("script", "os.execute")):
            doc = asset()
            doc[key] = value
            with self.assertRaises(Refusal):
                validate_asset(doc)

    def test_duplicate_identity_and_cycles(self):
        for mode in ("duplicate", "cycle", "missing"):
            doc = asset()
            if mode == "duplicate":
                doc["objects"].append(copy.deepcopy(doc["objects"][0]))
            else:
                doc["objects"][0]["parent"] = "fixture.root" if mode == "cycle" else "missing"
            with self.assertRaises(Refusal):
                validate_asset(doc)

    def test_new_instance_id_preserves_asset_identity(self):
        doc = asset()
        second = copy.deepcopy(doc["objects"][0])
        second["id"] = "fixture.second"
        doc["objects"].append(second)
        validate_asset(doc)

    def test_components_never_execute(self):
        doc = asset()
        doc["objects"][0]["components"] = [{"type": "game.battery.v1"}]
        with self.assertRaises(Refusal) as found:
            validate_asset(doc)
        self.assertEqual(found.exception.finding["rule_id"], "component.unregistered")

    def test_nonfinite_refused(self):
        doc = asset()
        doc["objects"][0]["transform"][0] = float("nan")
        with self.assertRaises(Refusal):
            validate_asset(doc)

    def test_optional_annotations_are_inert(self):
        doc = asset()
        doc["annotations"] = {"game.notes": {"script": "never execute"}}
        validate_asset(doc)

    def test_layout_only_graph_change(self):
        before = graph()
        after = copy.deepcopy(before)
        after["layout"]["sum"] = [500, 700]
        self.assertEqual(graph_semantic_digest(before), graph_semantic_digest(after))
        after["nodes"][0]["layout"] = "an actual semantic field"
        self.assertNotEqual(graph_semantic_digest(before), graph_semantic_digest(after))

    def test_graph_save_reload_preserves_ports_definitions_and_connections(self):
        before = graph()
        after = json.loads(canonical(before))
        self.assertEqual(before, after)
        for target in ("definitions", "connections"):
            after = copy.deepcopy(before)
            after[target].pop()
            self.assertNotEqual(graph_semantic_digest(before), graph_semantic_digest(after))

    def test_strict_json(self):
        with tempfile.TemporaryDirectory() as root:
            source = Path(root) / "bad.json"
            for text in ('{"x":1,"x":2}', '{"x":NaN}'):
                source.write_text(text)
                with self.assertRaises(ValueError):
                    read_json(source)
                with self.assertRaises(ValueError):
                    parse_json(text.encode())

    def test_label_matches_schema(self):
        value = asset()
        value["objects"][0]["label"] = 123
        with self.assertRaises(Refusal):
            validate_asset(value)


class Service(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.token = "a" * 48
        atomic_json(self.root / "asset.json", asset())
        self.service = MockService(self.root, self.token)

    def tearDown(self):
        self.service.close()
        self.temp.cleanup()

    def call(self, op, **params):
        return self.service.request({"op": op, "params": params, "token": self.token})

    def submit(self):
        return self.call("build", source="asset.json", revision=1)["job_id"]

    def finish(self, job_id):
        for _ in range(3):
            result = self.call("job.status", job_id=job_id)
        self.assertEqual(result["status"], "succeeded", result)
        return result

    def test_capabilities_never_claim_native_support(self):
        caps = self.call("capabilities")
        self.assertEqual(caps["mode"], "MOCK")
        self.assertFalse(caps["engine_preview"])
        self.assertEqual(self.call("registry")["components"], [])

    def test_invalid_token_and_operation(self):
        with self.assertRaises(Refusal):
            self.service.request({"token": "wrong", "op": "build"})
        with self.assertRaises(Refusal):
            self.call("python.exec", code="anything")

    def test_one_service_owner(self):
        with self.assertRaises(Refusal):
            MockService(self.root, self.token)

    def test_recovery_error_releases_project_lock(self):
        self.service.close()
        with patch.object(MockService, "_recover", side_effect=ValueError("corrupt receipt")):
            with self.assertRaises(ValueError):
                MockService(self.root, self.token)
        self.service = MockService(self.root, self.token)

    def test_mock_state_cannot_be_a_dependency(self):
        value = asset()
        path = self.service.root / "reserved.json"
        atomic_json(path, {})
        value["dependencies"] = [".mock-author/reserved.json"]
        atomic_json(self.root / "asset.json", value)
        with self.assertRaises(Refusal) as found:
            self.submit()
        self.assertEqual(found.exception.finding["rule_id"], "path.reserved")
        if os.name != "nt":
            (self.root / "alias.json").symlink_to(path)
            value["dependencies"] = ["alias.json"]
            atomic_json(self.root / "asset.json", value)
            with self.assertRaises(Refusal) as found:
                self.submit()
            self.assertEqual(found.exception.finding["rule_id"], "path.reserved")

    def test_atomic_publish_and_repeat_identity(self):
        first = self.submit()
        self.assertIsNone(self.service.current())
        receipt = self.finish(first)
        self.assertEqual(self.service.current()["job_id"], first)
        second = self.submit()
        self.assertEqual(self.service.current()["job_id"], first)
        self.assertEqual(receipt["build_identity"], self.finish(second)["build_identity"])

    def test_cancellation_at_every_phase_preserves_valid_generation(self):
        first = self.submit()
        self.finish(first)
        for steps in (0, 1, 2):
            job = self.submit()
            for _ in range(steps):
                self.call("job.status", job_id=job)
            self.assertEqual(self.call("job.cancel", job_id=job)["status"], "cancelled")
            self.assertEqual(self.call("job.status", job_id=job)["status"], "cancelled")
            self.assertEqual(self.service.current()["job_id"], first)

    def test_stale_source_even_without_revision_bump(self):
        first = self.submit()
        self.finish(first)
        job = self.submit()
        source = asset()
        source["objects"][0]["transform"][12] = 17
        atomic_json(self.root / "asset.json", source)
        self.assertEqual(self.call("job.status", job_id=job)["status"], "stale")
        self.assertEqual(self.service.current()["job_id"], first)

    def test_revision_mismatch(self):
        with self.assertRaises(Refusal):
            self.call("build", source="asset.json", revision=0)

    def test_changed_dependency(self):
        (self.root / "mesh.glb").write_bytes(b"synthetic")
        source = asset()
        source["dependencies"] = ["mesh.glb"]
        atomic_json(self.root / "asset.json", source)
        first = self.submit()
        self.finish(first)
        job = self.submit()
        (self.root / "mesh.glb").write_bytes(b"changed")
        self.assertEqual(self.call("job.status", job_id=job)["status"], "stale")
        self.assertEqual(self.service.current()["job_id"], first)

    def test_invalid_new_export_preserves_valid_generation(self):
        first = self.submit()
        self.finish(first)
        source = asset()
        source["required_schemas"] = ["unregistered"]
        atomic_json(self.root / "asset.json", source)
        with self.assertRaises(Refusal):
            self.submit()
        self.assertEqual(self.service.current()["job_id"], first)

    def test_output_corruption_preserves_valid_generation(self):
        first = self.submit()
        self.finish(first)
        job = self.submit()
        self.call("job.status", job_id=job)
        self.call("job.status", job_id=job)
        candidate = self.service.root / "jobs" / job / "outputs" / "snapshot.mock.json"
        candidate.write_bytes(b"corrupt")
        self.assertEqual(self.call("job.status", job_id=job)["status"], "failed")
        self.assertEqual(self.service.current()["job_id"], first)

    def test_hold_old_generation_and_stale_camera(self):
        first = self.submit()
        self.finish(first)
        preview = self.call("preview.open", job_id=first)["preview_id"]
        second = self.submit()
        self.finish(second)
        frame = self.call("preview.frame", preview_id=preview)
        self.assertEqual(frame["job_id"], first)
        self.assertIsNone(frame["depth"])
        self.assertFalse(frame["rendered"])
        self.call("preview.update", preview_id=preview, camera_revision=3)
        with self.assertRaises(Refusal):
            self.call("preview.update", preview_id=preview, camera_revision=2)
        self.assertEqual(self.call("preview.reset", preview_id=preview)["epoch"], 1)
        self.call("preview.close", preview_id=preview)
        with self.assertRaises(Refusal):
            self.call("preview.frame", preview_id=preview)

    def test_input_snapshot_corruption_refused(self):
        job = self.submit()
        self.call("job.status", job_id=job)
        directory = self.service.root / "jobs" / job / "inputs"
        next(directory.iterdir()).write_bytes(b"corrupted immutable input")
        result = self.call("job.status", job_id=job)
        self.assertEqual(result["status"], "failed")
        self.assertEqual(result["findings"][0]["rule_id"], "input.corrupt")
        self.assertIsNone(self.service.current())

    def test_multiple_preview_isolation(self):
        job = self.submit()
        self.finish(job)
        a = self.call("preview.open", job_id=job)["preview_id"]
        b = self.call("preview.open", job_id=job)["preview_id"]
        self.call("preview.update", preview_id=a, camera_revision=2)
        self.assertEqual(self.call("preview.frame", preview_id=b)["camera_revision"], 0)

    def test_recover_crash_before_publication(self):
        first = self.submit()
        self.finish(first)
        job = self.submit()
        self.service.lock.close()  # Simulate OS dropping process-owned lock.
        self.service.closed = True
        self.service = MockService(self.root, self.token)
        self.assertEqual(self.call("job.status", job_id=job)["status"], "failed")
        self.assertEqual(self.service.current()["job_id"], first)

    def test_recover_crash_after_pointer_commit(self):
        job = self.submit()
        self.call("job.status", job_id=job)
        self.call("job.status", job_id=job)
        real_atomic = atomic_json
        def crash_after_commit(path, value):
            real_atomic(path, value)
            if Path(path).name == "current.json":
                raise SystemExit("simulated crash")
        with patch("mock_service.atomic_json", crash_after_commit):
            with self.assertRaises(SystemExit):
                self.call("job.status", job_id=job)
        self.service.lock.close()
        self.service.closed = True
        self.service = MockService(self.root, self.token)
        self.assertEqual(self.call("job.status", job_id=job)["status"], "succeeded")

    def test_shutdown_cancels_owned_jobs(self):
        job = self.submit()
        self.service.close()
        receipt = read_json(self.service.root / "jobs" / job / "receipt.json")
        self.assertEqual(receipt["status"], "cancelled")

    def test_path_escape_and_symlink(self):
        for path in ("../asset.json", "/etc/passwd", "C:/outside", "..\\outside"):
            with self.assertRaises(Refusal):
                project_path(self.root, path)
        if os.name != "nt":
            (self.root / "escape").symlink_to("/etc/passwd")
            with self.assertRaises(Refusal):
                project_path(self.root, "escape")

    def test_stdio_crash_recovery(self):
        self.service.close()
        env = {**os.environ, "LUMINUMBRA_MOCK_TOKEN": self.token}
        with subprocess.Popen([sys.executable, str(Path(__file__).with_name("mock_service.py")),
                               "--project", str(self.root)], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env) as process:
            try:
                process.stdin.write(json.dumps({"token": self.token, "op": "build",
                                                "params": {"source": "asset.json", "revision": 1}}) + "\n")
                process.stdin.flush()
                result = json.loads(process.stdout.readline())
                self.assertTrue(result["ok"], result)
                job = result["result"]["job_id"]
            finally:
                process.terminate()  # Only our isolated child; no external process discovery.
                process.wait(timeout=10)
        self.service = MockService(self.root, self.token)
        self.assertEqual(self.call("job.status", job_id=job)["status"], "failed")


class ImporterEvidence(unittest.TestCase):
    def test_export_comparison_reads_serialized_glbs(self):
        with tempfile.TemporaryDirectory() as root:
            run(root)
            result = compare(Path(root) / "clean_static.glb", Path(root) / "clean_static.repeat.glb")
            self.assertTrue(result["exact_bytes_equal"])
            self.assertTrue(result["canonical_document_and_buffer_equal"])
            different = compare(Path(root) / "clean_static.glb", Path(root) / "transformed_instances_unknown_schema.glb")
            self.assertFalse(different["canonical_document_and_buffer_equal"])

    def test_fixture_findings_and_repeatability(self):
        with tempfile.TemporaryDirectory() as root:
            records = {item["fixture"]: item for item in run(root)}
        for name in ("transformed_instances_unknown_schema", "child_before_parent", "matrix_joint_bind", "step_animation", "cubicspline_animation"):
            # Record the selected validator's findings, including future fixes.
            # An accepted unsupported asset is evidence, never a success criterion.
            self.assertIsInstance(records[name]["findings"], list)
            self.assertTrue(records[name]["exact_repeat"])
        self.assertEqual(records["unknown_required_extension"]["findings"][0]["rule_id"], "extension.unsupported")


if __name__ == "__main__":
    unittest.main()
