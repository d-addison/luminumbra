"""Fault injection for publication; native compiler acceptance is a separate lane."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import zipfile
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from luminumbra_author import service as service_module
from luminumbra_author.__main__ import dispatch
from luminumbra_author.contracts import (ASSET, PROFILE, TOOLCHAIN, Refusal, atomic_json,
                                         canonical, digest, read_json)
from luminumbra_author.formats import mesh_info, clip_info, validate_glb
from luminumbra_author.service import BuildService, TERMINAL
from package import build as package_build


def glb(document=None):
    document = document or {"asset": {"version": "2.0"}, "scenes": [{"nodes": []}]}
    payload = canonical(document)
    payload += b" " * (-len(payload) % 4)
    return struct.pack("<4sIIII", b"glTF", 2, 20 + len(payload), len(payload), 0x4e4f534a) + payload


def triangle():
    # Independent known wire fixture: one triangle, 3 vertices, no skin.
    header = struct.pack("<4sII4f", b"LMSH", 3, 3, 0.5, 0.5, 0, 0.7071068)
    vertices = b"".join(struct.pack("<8f", x, y, 0, 0, 0, 1, x, y)
                        for x, y in ((0, 0), (1, 0), (0, 1)))
    return header + vertices + struct.pack("<3I", 0, 1, 2)


class Project(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.project = self.root / "project"
        self.project.mkdir()
        self.tools = self.root / "installed"
        self.tools.mkdir()
        (self.tools / "processor").write_bytes(b"Unit test compiler stand-in, never executed.")
        self.toolchain = self.tools / "toolchain.json"
        atomic_json(self.toolchain, {"schema": TOOLCHAIN, "id": "test.compiler",
                    "processor": "processor", "files": {"processor": digest((self.tools / "processor").read_bytes())}})
        self.asset = {"schema": ASSET, "profile": PROFILE, "asset_id": "test.triangle", "revision": 1,
                      "source": "triangle.glb", "dependencies": ["source.blend"],
                      "exporter": {"id": "test.fixture", "sha256": digest(b"fixture")}}
        atomic_json(self.project / "asset.json", self.asset)
        (self.project / "triangle.glb").write_bytes(glb())
        (self.project / "source.blend").write_bytes(b"dependency, never interpreted or executed")

    def open(self):
        service = BuildService(self.project, self.toolchain)
        self.addCleanup(service.close)
        return service

    def compile_valid(self, job_id, source, outputs, emit_lods):
        (outputs / "asset.lmesh").write_bytes(triangle())

    def wait(self, service, job):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            result = service.status(job["job_id"])
            if result["status"] in TERMINAL:
                return result
            time.sleep(0.005)
        self.fail("Build did not reach a terminal state")

    def success(self, service):
        with patch.object(service, "_compile", side_effect=self.compile_valid):
            result = self.wait(service, service.submit("asset.json", 1))
        self.assertEqual(result["status"], "succeeded", result)
        return result

    def test_compiles_snapshot_and_retains_both_generations(self):
        service = self.open()
        first = self.success(service)
        first_manifest = service.generation(first["job_id"])
        second = self.success(service)
        self.assertNotEqual(first["job_id"], second["job_id"])
        self.assertEqual(first["build_identity"], second["build_identity"])
        self.assertEqual(first_manifest, service.generation(first["job_id"]))
        self.assertEqual(service.current()["job_id"], second["job_id"])
        self.assertEqual(second["outputs"]["asset.lmesh"]["triangles"], 1)
        self.assertEqual(second["outputs"]["asset.lmesh"]["bytes"], 136)
        self.assertFalse(second["rendered"])
        self.assertFalse(service.capabilities()["engine_preview"])

    def test_dependency_changes_build_identity(self):
        service = self.open()
        first = self.success(service)
        (self.project / "source.blend").write_bytes(b"new source dependency")
        second = self.success(service)
        self.assertNotEqual(first["build_identity"], second["build_identity"])

    def test_partial_and_malformed_outputs_preserve_previous(self):
        service = self.open()
        previous = self.success(service)
        pointer = service.current()
        for payload in (b"", b"LMSH", triangle()[:-1], triangle() + b"extra"):
            def compile_bad(job_id, source, outputs, emit_lods):
                (outputs / "asset.lmesh").write_bytes(payload)
            with patch.object(service, "_compile", side_effect=compile_bad):
                result = self.wait(service, service.submit("asset.json", 1))
            self.assertEqual(result["status"], "failed")
            self.assertEqual(service.current(), pointer)
        self.assertEqual(service.generation(previous["job_id"])["outputs"], previous["outputs"])

    def test_cancel_during_extraction_and_compilation(self):
        service = self.open()
        self.success(service)
        pointer = service.current()
        for phase in ("_snapshot", "_compile"):
            entered, release = threading.Event(), threading.Event()
            original = getattr(service, phase)
            def blocked(*args, **kwargs):
                entered.set()
                self.assertTrue(release.wait(5))
                if phase == "_compile":
                    return self.compile_valid(*args, **kwargs)
                return original(*args, **kwargs)
            with patch.object(service, phase, side_effect=blocked):
                job = service.submit("asset.json", 1)
                self.assertTrue(entered.wait(5))
                service.cancel(job["job_id"])
                release.set()
                result = self.wait(service, job)
            self.assertEqual(result["status"], "cancelled", result)
            self.assertEqual(service.current(), pointer)

    def test_source_revision_and_dependency_edits_during_compile_are_stale(self):
        service = self.open()
        self.success(service)
        pointer = service.current()
        for name in ("asset.json", "triangle.glb", "source.blend"):
            original = (self.project / name).read_bytes()
            def compile_edit(*args):
                self.compile_valid(*args)
                if name == "asset.json":
                    changed = {**self.asset, "revision": 2}
                    atomic_json(self.project / name, changed)
                elif name == "triangle.glb":
                    (self.project / name).write_bytes(glb({"asset": {"version": "2.0", "generator": "changed"}}))
                else:
                    (self.project / name).write_bytes(b"changed")
            with patch.object(service, "_compile", side_effect=compile_edit):
                result = self.wait(service, service.submit("asset.json", 1))
            self.assertEqual(result["status"], "stale", result)
            self.assertEqual(service.current(), pointer)
            (self.project / name).write_bytes(original)

    def test_tool_change_and_input_tampering_refuse_publication(self):
        service = self.open()
        self.success(service)
        pointer = service.current()
        for target in ("tool", "input"):
            original = (self.tools / "processor").read_bytes()
            def corrupt(job_id, source, outputs, emit_lods):
                self.compile_valid(job_id, source, outputs, emit_lods)
                (self.tools / "processor" if target == "tool" else source).write_bytes(b"changed")
            with patch.object(service, "_compile", side_effect=corrupt):
                result = self.wait(service, service.submit("asset.json", 1))
            self.assertEqual(result["status"], "failed", result)
            self.assertEqual(service.current(), pointer)
            (self.tools / "processor").write_bytes(original)

    def test_publication_io_failure_retains_pointer(self):
        service = self.open()
        self.success(service)
        pointer = service.current()
        write = service_module.atomic_json
        def fail_pointer(path, value):
            if Path(path).name == "current.json":
                raise OSError("injected atomic pointer failure")
            write(path, value)
        with patch.object(service, "_compile", side_effect=self.compile_valid), \
             patch.object(service_module, "atomic_json", side_effect=fail_pointer):
            result = self.wait(service, service.submit("asset.json", 1))
        self.assertEqual(result["status"], "failed")
        self.assertEqual(service.current(), pointer)

    def test_cancel_when_candidate_is_ready_to_publish(self):
        service = self.open()
        self.success(service)
        pointer = service.current()
        ready, release = threading.Event(), threading.Event()
        change = service._change
        def pause(job_id, **values):
            change(job_id, **values)
            if values.get("status") == "publishing":
                ready.set()
                self.assertTrue(release.wait(5))
        with patch.object(service, "_change", side_effect=pause), \
             patch.object(service, "_compile", side_effect=self.compile_valid):
            job = service.submit("asset.json", 1)
            self.assertTrue(ready.wait(5))
            service.cancel(job["job_id"])
            release.set()
            result = self.wait(service, job)
        self.assertEqual(result["status"], "cancelled")
        self.assertEqual(service.current(), pointer)

    def test_recovery_distinguishes_committed_and_orphaned_jobs(self):
        service = self.open()
        first = self.success(service)
        second = self.success(service)
        service.close()
        for job in (first, second):
            job["status"] = "publishing"
            atomic_json(service.root / "jobs" / job["job_id"] / "receipt.json", job)
        recovered = self.open()
        self.assertEqual(recovered.status(first["job_id"])["status"], "failed")
        self.assertEqual(recovered.status(second["job_id"])["status"], "succeeded")
        self.assertEqual(recovered.generation()["job_id"], second["job_id"])

    def test_exclusive_project_lock_and_cleanup(self):
        first = self.open()
        with self.assertRaises(Refusal) as refusal:
            BuildService(self.project, self.toolchain)
        self.assertEqual(refusal.exception.finding["rule_id"], "session.busy")
        first.close()
        self.open()

    def test_unknown_contracts_and_unsafe_paths_never_compile(self):
        service = self.open()
        changes = [{"schema": "future"}, {"profile": "future"}, {"components": []},
                   {"required_schemas": ["future"]}, {"settings": {"shell": "anything"}},
                   {"source": "../escape.glb"}, {"source": "C:/escape.glb"},
                   {"dependencies": [".luminumbra-author/current.json"]}]
        for changeset in changes:
            atomic_json(self.project / "asset.json", {**self.asset, **changeset})
            with patch.object(service, "_compile") as compiler:
                result = self.wait(service, service.submit("asset.json", 1))
            self.assertEqual(result["status"], "failed", result)
            compiler.assert_not_called()
        with self.assertRaises(Refusal):
            dispatch(service, "preview.open", {})

    def test_corrupt_retained_generation_is_detected(self):
        service = self.open()
        job = self.success(service)
        output = service.root / "generations" / job["job_id"] / "asset.lmesh"
        output.write_bytes(b"corrupt")
        with self.assertRaises(Refusal):
            service.generation(job["job_id"])

    def test_installed_archive_runs_without_source_import_path_and_reports_errors(self):
        archive = package_build(self.root / "luminumbra-author")
        with zipfile.ZipFile(archive) as package:
            self.assertIn(b"MIT License", package.read("LICENSE"))
        env = os.environ.copy()
        env.pop("PYTHONPATH", None)
        command = [sys.executable, str(archive), "--project", str(self.project), "--toolchain", str(self.toolchain)]
        result = subprocess.run(command + ["capabilities"], cwd=self.root, env=env,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(json.loads(result.stdout)["result"]["engine_compilation"])
        result = subprocess.run(command + ["validate", "missing.json"], cwd=self.root, env=env,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 1)
        self.assertFalse(json.loads(result.stdout)["ok"])

    def test_authenticated_pipe_refuses_bad_token_and_unknown_operation(self):
        archive = package_build(self.root / "luminumbra-author")
        token = "test-only-session-token-0123456789abcdef"
        requests = [
            {"id": 1, "token": "invalid", "op": "capabilities", "params": {}},
            {"id": 2, "token": token, "op": "capabilities", "params": {}},
            {"id": 3, "token": token, "op": "preview.open", "params": {}},
        ]
        command = [sys.executable, str(archive), "--project", str(self.project),
                   "--toolchain", str(self.toolchain), "serve"]
        result = subprocess.run(command, input="\n".join(json.dumps(r) for r in requests) + "\n",
                                env={**os.environ, "LUMINUMBRA_AUTHOR_TOKEN": token},
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual([r["ok"] for r in rows], [False, True, False])
        self.assertEqual(rows[0]["findings"][0]["rule_id"], "session.unauthorized")
        self.assertEqual(rows[2]["findings"][0]["rule_id"], "operation.unsupported")


class Formats(unittest.TestCase):
    def test_known_mesh_and_invalid_extents_indices_attributes(self):
        expected = triangle()
        self.assertEqual(mesh_info(expected)["vertices"], 3)
        for data in (expected[:size] for size in range(len(expected))):
            with self.assertRaises(Refusal):
                mesh_info(data)
        for offset, value in ((4, 0xffffffff), (124, 3), (28, 0x7fc00000)):
            data = bytearray(expected)
            struct.pack_into("<I", data, offset, value)
            with self.assertRaises(Refusal):
                mesh_info(data)

    def test_glb_external_required_metadata_and_invalid_chunks(self):
        validate_glb(glb())
        for document in ({"asset": []}, {"asset": {"version": "3.0"}},
                         {"asset": {"version": "2.0"}, "buffers": [{"uri": "../escape.bin"}]},
                         {"asset": {"version": "2.0"}, "extensionsRequired": ["unknown"]},
                         {"asset": {"version": "2.0"}, "extras": {"luminumbra": {"schema": "future", "required": True}}},
                         {"asset": {"version": "2.0"}, "extras": {"luminumbra.schema": "unknown"}}):
            with self.assertRaises(Refusal):
                validate_glb(glb(document))
        for data in (glb()[:-1], glb() + b"more", b"not a GLB snapshot"):
            with self.assertRaises(Refusal):
                validate_glb(data)

    def test_clip_schema_interpolation_and_skeleton_binding(self):
        # One root translation track, STEP, independently encoded.
        data = struct.pack("<4sIIf5I8f", b"LANM", 2, 1, 1, 42, 0, 2, 3, 1,
                           0, 1, 0, 0, 0, 0, 2, 0)
        self.assertEqual(clip_info(data, [42])["interpolation_modes"], [1])
        for bad in (data[:-1], data + b"extra"):
            with self.assertRaises(Refusal):
                clip_info(bad, [42])
        with self.assertRaises(Refusal):
            clip_info(data, [43])
        for offset, value in ((4, 3), (32, 999), (40, 0)):
            bad = bytearray(data)
            struct.pack_into("<I", bad, offset, value)
            with self.assertRaises(Refusal):
                clip_info(bad, [42])


class Supervisor(unittest.TestCase):
    def run_child(self, script, timeout=5, cancel=False):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "result.json"
            env = {**os.environ, "PYTHONPATH": str(Path(__file__).resolve().parents[1])}
            command = [sys.executable, "-m", "luminumbra_author", "_compiler", str(output), str(timeout),
                       sys.executable, "-c", script]
            child = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, env=env)
            if cancel:
                child.stdin.close()
            try:
                child.wait(timeout=10)
                self.assertEqual(child.returncode, 0, child.stderr.read().decode())
                return read_json(output)
            finally:
                child.stdin.close()
                if child.poll() is None:
                    child.kill()
                    child.wait()
                child.stdout.close()
                child.stderr.close()

    def test_logs_are_drained_and_bounded(self):
        result = self.run_child("print('x' * 200000)")
        self.assertEqual(result["returncode"], 0)
        self.assertEqual(len(result["log"]), 65536)
        self.assertTrue(result["log_truncated"])
        self.assertGreater(result["log_bytes"], 200000)

    def test_timeout_kills_compiler(self):
        result = self.run_child("import time; time.sleep(60)", timeout=0.1)
        self.assertTrue(result["timed_out"])
        self.assertNotEqual(result["returncode"], 0)

    def test_parent_pipe_closure_kills_compiler(self):
        result = self.run_child("import time; time.sleep(60)", cancel=True)
        self.assertTrue(result["cancelled"])
        self.assertNotEqual(result["returncode"], 0)


if __name__ == "__main__":
    unittest.main()
