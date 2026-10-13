"""Fault injection for publication; native compiler acceptance is a separate lane."""
import json
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import zipfile
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from luminumbra_author import service as service_module
from luminumbra_author.__main__ import dispatch
from luminumbra_author.contracts import (ASSET, PROFILE, PREFAB_PROFILE, TOOLCHAIN, Refusal, atomic_json,
                                         canonical, digest, read_json, validate_asset)
from luminumbra_author.formats import mesh_info, clip_info, validate_glb
from luminumbra_author.service import BuildService, TERMINAL
from package import build as package_build


def glb(document=None):
    document = document or {"asset": {"version": "2.0"}, "scenes": [{"nodes": []}]}
    payload = canonical(document)
    payload += b" " * (-len(payload) % 4)
    return struct.pack("<4sIIII", b"glTF", 2, 20 + len(payload), len(payload), 0x4e4f534a) + payload


def triangle(count=1):
    # Independent known wire fixture: repeated triangles, 3 vertices, no skin.
    header = struct.pack("<4sII4f", b"LMSH", 3, count * 3, 0.5, 0.5, 0, 0.7071068)
    vertices = b"".join(struct.pack("<8f", x, y, 0, 0, 0, 1, x, y)
                        for x, y in ((0, 0), (1, 0), (0, 1)))
    return header + vertices + struct.pack("<3I", 0, 1, 2) * count


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

    def compile_valid(self, job_id, source, outputs, emit_lods, max_tris=None):
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

    def test_max_tris_contract_bounds_and_types(self):
        for cap in (64, 1_000_000):
            with self.subTest(cap=cap):
                document = {**self.asset, "settings": {"emit_lods": True, "max_tris": cap}}
                before = canonical(document)
                self.assertIs(validate_asset(document), document)
                self.assertEqual(canonical(document), before)
        service = self.open()
        for cap in (63, 1_000_001, True, False, 64.0, "64", None, [], {}, -1, 10**400):
            with self.subTest(cap=cap):
                atomic_json(self.project / "asset.json", {**self.asset, "settings": {"max_tris": cap}})
                with self.assertRaises(Refusal) as caught:
                    service.validate("asset.json")
                self.assertEqual(caught.exception.finding["rule_id"], "settings.invalid")
                self.assertEqual(caught.exception.finding["field"], "settings.max_tris")
                with patch.object(service, "_compile") as compiler:
                    result = self.wait(service, service.submit("asset.json", 1))
                self.assertEqual(result["status"], "failed", result)
                self.assertEqual(result["findings"][0]["rule_id"], "settings.invalid")
                compiler.assert_not_called()
                self.assertIsNone(service.current())

    def test_max_tris_exact_compiler_arguments(self):
        service = self.open()
        source, outputs = self.root / "input.glb", self.root / "outputs"
        for emit_lods, cap, arguments in (
                (False, None, []), (True, None, ["--emit-lods"]),
                (False, 64, ["--max-tris", "64"]),
                (True, 1_000_000, ["--emit-lods", "--max-tris", "1000000"])):
            with self.subTest(emit_lods=emit_lods, cap=cap), patch.object(service, "_invoke") as invoke:
                service._compile("unused", source, outputs, emit_lods, cap)
                invoke.assert_called_once_with("unused", source, outputs / "asset.lmesh", arguments)

    def test_max_tris_actual_count_and_failed_publication(self):
        service = self.open()
        previous = self.success(service)
        pointer_path = service.root / "current.json"
        pointer = pointer_path.read_bytes()
        generation = service.root / "generations" / previous["job_id"]
        retained = {path.name: path.read_bytes() for path in generation.iterdir()}
        self.asset["settings"] = {"max_tris": 64}
        atomic_json(self.project / "asset.json", self.asset)
        for count in (65, 63, 64):
            def compile_count(job_id, source, outputs, emit_lods, max_tris):
                self.assertFalse(emit_lods)
                self.assertEqual(max_tris, 64)
                (outputs / "asset.lmesh").write_bytes(triangle(count))
                if count > 64:
                    (outputs / "asset.lod1.lmesh").write_bytes(triangle(32))
                    (outputs / "asset.walk.lanim").write_bytes(b"unpublished animation output")
            with self.subTest(count=count), patch.object(service, "_compile", side_effect=compile_count):
                result = self.wait(service, service.submit("asset.json", 1))
            self.assertEqual(result["settings"], {"max_tris": 64})
            self.assertEqual(read_json(service.root / "jobs" / result["job_id"] / "receipt.json"), result)
            if count > 64:
                self.assertEqual(result["status"], "failed", result)
                self.assertEqual(result["findings"][0]["rule_id"], "output.budget_exceeded")
                self.assertEqual(result["findings"][0]["field"], "asset.lmesh")
                self.assertEqual(pointer_path.read_bytes(), pointer)
                self.assertFalse((service.root / "generations" / result["job_id"]).exists())
                job = service.root / "jobs" / result["job_id"]
                self.assertFalse((job / "outputs").exists())
                self.assertEqual((job / "source.glb").read_bytes(), (self.project / "triangle.glb").read_bytes())
                self.assertEqual({path.name for path in (job / "inputs").iterdir()},
                                 set(result["input_hashes"].values()))
                for name, expected in result["input_hashes"].items():
                    self.assertEqual((job / "inputs" / expected).read_bytes(), (self.project / name).read_bytes())
            else:
                self.assertEqual(result["status"], "succeeded", result)
                self.assertEqual(result["outputs"]["asset.lmesh"]["triangles"], count)
                self.assertEqual(service.current()["job_id"], result["job_id"])
            self.assertEqual({path.name: path.read_bytes() for path in generation.iterdir()}, retained)

    def assert_cleanup_link_refusal(self, component, make_link):
        service = self.open()
        previous = self.success(service)
        pointer = (service.root / "current.json").read_bytes()
        generation = service.root / "generations" / previous["job_id"]
        retained = {path.name: path.read_bytes() for path in generation.iterdir()}
        external = self.root / "outside-staging"
        self.asset["settings"] = {"max_tris": 64}
        atomic_json(self.project / "asset.json", self.asset)
        canary = triangle(1)

        def compile_replaced(job_id, source, outputs, emit_lods, max_tris):
            target = outputs.parent if component == "job" else outputs
            backup = self.root / "owned-staging"
            target.rename(backup)
            # Matching snapshots reproduce the reported postcompile attack.
            shutil.copytree(backup, external)
            external_outputs = external / "outputs" if component == "job" else external
            (external_outputs / "asset.lmesh").write_bytes(triangle(65))
            (external_outputs / "asset.lod1.lmesh").write_bytes(canary)
            make_link(target, external)
            self.addCleanup(target.rmdir if os.name == "nt" else target.unlink)

        with patch.object(service, "_compile", side_effect=compile_replaced):
            result = self.wait(service, service.submit("asset.json", 1))
        self.assertEqual(result["status"], "failed", result)
        self.assertEqual(result["findings"][0]["rule_id"], "output.budget_exceeded")
        self.assertEqual(result["cleanup_findings"][0]["rule_id"], "output.cleanup_refused")
        self.assertEqual(read_json(service.root / "jobs" / result["job_id"] / "receipt.json"), result)
        external_outputs = external / "outputs" if component == "job" else external
        self.assertEqual((external_outputs / "asset.lod1.lmesh").read_bytes(), canary)
        self.assertEqual((external_outputs / "asset.lmesh").read_bytes(), triangle(65))
        self.assertEqual((service.root / "current.json").read_bytes(), pointer)
        self.assertEqual({path.name: path.read_bytes() for path in generation.iterdir()}, retained)

    def directory_symlink(self, link, target):
        try:
            link.symlink_to(target, target_is_directory=True)
        except (OSError, NotImplementedError) as error:
            self.skipTest(f"Directory symlink capability unavailable: {error}")

    def require_directory_symlink(self):
        probe = self.root / "symlink-probe"
        self.directory_symlink(probe, self.project)
        (probe.rmdir if os.name == "nt" else probe.unlink)()

    def test_budget_cleanup_refuses_replaced_job_symlink_and_preserves_outside_canary(self):
        self.require_directory_symlink()
        self.assert_cleanup_link_refusal("job", self.directory_symlink)

    def test_budget_cleanup_refuses_outputs_symlink_and_preserves_outside_canary(self):
        self.require_directory_symlink()
        self.assert_cleanup_link_refusal("outputs", self.directory_symlink)

    def test_budget_cleanup_junction_or_reparse_attribute_preserves_outside_canary(self):
        # Exercise a real junction whenever this host can create one. All other
        # hosts still exercise the Windows reparse-attribute refusal, never skip.
        target = self.root / "junction-target"
        target.mkdir()
        probe = self.root / "junction-probe"
        if os.name == "nt":
            result = subprocess.run(["cmd", "/c", "mklink", "/J", str(probe), str(target)],
                                    capture_output=True, timeout=10)
            if result.returncode == 0:
                probe.rmdir()

                def junction(link, destination):
                    result = subprocess.run(["cmd", "/c", "mklink", "/J", str(link), str(destination)],
                                            capture_output=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stderr)

                self.assert_cleanup_link_refusal("job", junction)
                return

        service = self.open()
        external = target / "outputs"
        external.mkdir()
        canary = external / "canary"
        canary.write_bytes(b"outside must survive")
        self.asset["settings"] = {"max_tris": 64}
        atomic_json(self.project / "asset.json", self.asset)
        cleanup = service._cleanup_outputs
        lstat = Path.lstat
        observed = []

        def cleanup_reparse(job_id, output_identity):
            job_path = service.root / "jobs" / job_id

            def reparse(path, *args, **kwargs):
                info = lstat(path, *args, **kwargs)
                if path == job_path:
                    observed.append(path)
                    return SimpleNamespace(st_mode=info.st_mode, st_dev=info.st_dev, st_ino=info.st_ino,
                                           st_file_attributes=stat.FILE_ATTRIBUTE_REPARSE_POINT)
                return info

            with patch.object(Path, "lstat", reparse):
                cleanup(job_id, output_identity)

        def compile_over(*args):
            (args[2] / "asset.lmesh").write_bytes(triangle(65))

        with patch.object(service, "_compile", side_effect=compile_over), \
                patch.object(service, "_cleanup_outputs", side_effect=cleanup_reparse):
            result = self.wait(service, service.submit("asset.json", 1))
        self.assertTrue(observed, "The mocked reparse-attribute path must execute")
        self.assertEqual(result["findings"][0]["rule_id"], "output.budget_exceeded")
        self.assertEqual(result["cleanup_findings"][0]["rule_id"], "output.cleanup_refused")
        job_path = service.root / "jobs" / result["job_id"]
        self.assertEqual(read_json(job_path / "receipt.json"), result)
        self.assertEqual((job_path / "outputs" / "asset.lmesh").read_bytes(), triangle(65))
        self.assertEqual(canary.read_bytes(), b"outside must survive")

    def test_budget_cleanup_removes_nested_links_without_following_targets(self):
        self.require_directory_symlink()
        service = self.open()
        external = self.root / "outside-tree"
        external.mkdir()
        canary = external / "canary"
        canary.write_bytes(b"outside must survive")
        self.asset["settings"] = {"max_tris": 64}
        atomic_json(self.project / "asset.json", self.asset)
        validate_outputs = service._outputs

        def compile_over(*args):
            (args[2] / "asset.lmesh").write_bytes(triangle(65))

        def validate_and_add_tree(outputs, max_tris):
            try:
                return validate_outputs(outputs, max_tris)
            finally:
                nested = outputs / "nested"
                nested.mkdir()
                (nested / "unpublished").write_bytes(b"owned output")
                self.directory_symlink(nested / "linked-directory", external)
                try:
                    (nested / "linked-file").symlink_to(canary)
                    (nested / "dangling").symlink_to(external / "missing")
                except OSError as error:
                    self.skipTest(f"File symlink capability unavailable: {error}")

        with patch.object(service, "_compile", side_effect=compile_over), \
                patch.object(service, "_outputs", side_effect=validate_and_add_tree):
            result = self.wait(service, service.submit("asset.json", 1))
        self.assertEqual(result["findings"][0]["rule_id"], "output.budget_exceeded")
        self.assertNotIn("cleanup_findings", result)
        job_path = service.root / "jobs" / result["job_id"]
        self.assertFalse((job_path / "outputs").exists())
        self.assertTrue((job_path / "inputs").is_dir())
        self.assertEqual(read_json(job_path / "receipt.json"), result)
        self.assertEqual(canary.read_bytes(), b"outside must survive")

    def test_budget_cleanup_removes_ordinary_nested_directories(self):
        service = self.open()
        self.asset["settings"] = {"max_tris": 64}
        atomic_json(self.project / "asset.json", self.asset)
        cleanup = service._cleanup_outputs

        def cleanup_nested(job_id, output_identity):
            nested = service.root / "jobs" / job_id / "outputs" / "nested" / "child"
            nested.mkdir(parents=True)
            (nested / "unpublished").write_bytes(b"owned output")
            cleanup(job_id, output_identity)

        def compile_over(*args):
            (args[2] / "asset.lmesh").write_bytes(triangle(65))

        with patch.object(service, "_compile", side_effect=compile_over), \
                patch.object(service, "_cleanup_outputs", side_effect=cleanup_nested):
            result = self.wait(service, service.submit("asset.json", 1))
        self.assertEqual(result["findings"][0]["rule_id"], "output.budget_exceeded")
        self.assertNotIn("cleanup_findings", result)
        job_path = service.root / "jobs" / result["job_id"]
        self.assertFalse((job_path / "outputs").exists())
        self.assertTrue((job_path / "inputs").is_dir())
        self.assertEqual(read_json(job_path / "receipt.json"), result)

    def test_budget_cleanup_refuses_ancestor_swap_between_validation_and_open(self):
        self.require_directory_symlink()
        service = self.open()
        external = self.root / "outside-race"
        external.mkdir()
        (external / "outputs").mkdir()
        canary = external / "outputs" / "asset.lmesh"
        canary.write_bytes(triangle(65))
        self.asset["settings"] = {"max_tris": 64}
        atomic_json(self.project / "asset.json", self.asset)
        check_directory = service._check_cleanup_directory
        swapped = []

        def swap_after_check(path, expected):
            info = check_directory(path, expected)
            if path.name == "outputs" and not swapped:
                job_path = path.parent
                job_path.rename(self.root / "owned-before-race")
                self.directory_symlink(job_path, external)
                self.addCleanup(job_path.rmdir if os.name == "nt" else job_path.unlink)
                swapped.append(job_path)
            return info

        def compile_over(*args):
            (args[2] / "asset.lmesh").write_bytes(triangle(65))

        with patch.object(service, "_compile", side_effect=compile_over), \
                patch.object(service, "_check_cleanup_directory", side_effect=swap_after_check):
            result = self.wait(service, service.submit("asset.json", 1))
        self.assertTrue(swapped)
        self.assertEqual(result["findings"][0]["rule_id"], "output.budget_exceeded")
        self.assertEqual(result["cleanup_findings"][0]["rule_id"], "output.cleanup_refused")
        self.assertEqual(canary.read_bytes(), triangle(65))
        self.assertEqual((self.root / "owned-before-race" / "outputs" / "asset.lmesh").read_bytes(), triangle(65))

    @unittest.skipUnless(os.listdir in os.supports_fd and os.open in os.supports_dir_fd,
                         "Descriptor-relative walk is unavailable on this host")
    def test_budget_cleanup_uses_owned_descriptor_after_ancestor_swap_during_walk(self):
        self.require_directory_symlink()
        service = self.open()
        self.asset["settings"] = {"max_tris": 64}
        atomic_json(self.project / "asset.json", self.asset)
        external = self.root / "outside-during-walk"
        owned = self.root / "owned-during-walk"
        listdir = os.listdir
        swapped = []

        def swap_at_walk(path):
            if isinstance(path, int) and not swapped:
                job_path = service.root / "jobs" / next(reversed(service.cleanup_jobs))
                shutil.copytree(job_path, external)
                (external / "outputs" / "asset.lod1.lmesh").write_bytes(triangle(1))
                job_path.rename(owned)
                self.directory_symlink(job_path, external)
                self.addCleanup(job_path.rmdir if os.name == "nt" else job_path.unlink)
                swapped.append(job_path)
            return listdir(path)

        def compile_over(*args):
            (args[2] / "asset.lmesh").write_bytes(triangle(65))

        with patch.object(service, "_compile", side_effect=compile_over), \
                patch.object(os, "listdir", side_effect=swap_at_walk) as walk:
            with patch.object(os, "supports_fd", os.supports_fd | {walk}):
                result = self.wait(service, service.submit("asset.json", 1))
        self.assertTrue(swapped, "Swap must occur after directory descriptors have opened")
        self.assertEqual(result["findings"][0]["rule_id"], "output.budget_exceeded")
        self.assertNotIn("cleanup_findings", result)
        self.assertEqual((external / "outputs" / "asset.lmesh").read_bytes(), triangle(65))
        self.assertEqual((external / "outputs" / "asset.lod1.lmesh").read_bytes(), triangle(1))
        self.assertFalse((owned / "outputs").exists())
        self.assertTrue((owned / "inputs").is_dir())

    def test_max_tris_receipt_and_input_identity(self):
        service = self.open()
        results = []
        for cap in (64, 65, 1_000_000):
            self.asset["settings"] = {"emit_lods": False, "max_tris": cap}
            atomic_json(self.project / "asset.json", self.asset)
            with patch.object(service, "_compile", side_effect=self.compile_valid) as compiler:
                result = self.wait(service, service.submit("asset.json", 1))
            self.assertEqual(result["status"], "succeeded", result)
            self.assertEqual(compiler.call_args.args[-2:], (False, cap))
            self.assertEqual(result["settings"], self.asset["settings"])
            self.assertEqual(read_json(service.root / "jobs" / result["job_id"] / "receipt.json")["settings"],
                             self.asset["settings"])
            results.append(result)
        self.assertEqual(len({r["build_identity"] for r in results}), 3)
        self.assertEqual(len({r["input_hashes"]["asset.json"] for r in results}), 3)
        self.assertTrue(all(r["outputs"] == results[0]["outputs"] for r in results))

    def test_old_documents_keep_output_bytes_and_receipt_shape(self):
        service = self.open()
        for settings in (None, {}, {"emit_lods": False}, {"emit_lods": True}):
            document = dict(self.asset)
            if settings is not None:
                document["settings"] = settings
            atomic_json(self.project / "asset.json", document)
            def compile_legacy(job_id, source, outputs, emit_lods, max_tris):
                self.assertIsNone(max_tris)
                self.assertEqual(emit_lods, bool(settings and settings.get("emit_lods")))
                (outputs / "asset.lmesh").write_bytes(triangle(65))
                if emit_lods:
                    (outputs / "asset.lod1.lmesh").write_bytes(triangle(32))
                    (outputs / "asset.lod2.lmesh").write_bytes(triangle(48))
            with self.subTest(settings=settings), patch.object(service, "_compile", side_effect=compile_legacy):
                result = self.wait(service, service.submit("asset.json", 1))
            self.assertEqual(result["status"], "succeeded", result)
            self.assertNotIn("settings", result)
            generation = service.root / "generations" / result["job_id"]
            for name, count in (("asset.lmesh", 65), ("asset.lod1.lmesh", 32), ("asset.lod2.lmesh", 48)):
                if name in result["outputs"]:
                    payload = triangle(count)
                    self.assertEqual((generation / name).read_bytes(), payload)
                    expected = mesh_info(payload)
                    expected.pop("joint_ids")
                    self.assertEqual(result["outputs"][name], {"sha256": digest(payload), **expected})
            self.assertNotIn("settings", service.generation(result["job_id"]))

    def test_lod_counts_enforced_only_with_max_tris_before_publication(self):
        service = self.open()
        previous = self.success(service)
        generation = service.root / "generations" / previous["job_id"]
        retained = {path.name: path.read_bytes() for path in generation.iterdir()}
        pointer = (service.root / "current.json").read_bytes()
        for cap in (None, 64):
            self.asset["settings"] = {"emit_lods": True}
            if cap is not None:
                self.asset["settings"]["max_tris"] = cap
            atomic_json(self.project / "asset.json", self.asset)
            for excessive in (None, "asset.lod1.lmesh", "asset.lod2.lmesh"):
                def compile_lods(job_id, source, outputs, emit_lods, max_tris):
                    self.assertTrue(emit_lods)
                    self.assertEqual(max_tris, cap)
                    (outputs / "asset.lmesh").write_bytes(triangle(64))
                    for name in ("asset.lod1.lmesh", "asset.lod2.lmesh"):
                        (outputs / name).write_bytes(triangle(65 if name == excessive else 64))
                with self.subTest(cap=cap, excessive=excessive), \
                        patch.object(service, "_compile", side_effect=compile_lods):
                    result = self.wait(service, service.submit("asset.json", 1))
                job = service.root / "jobs" / result["job_id"]
                self.assertEqual(read_json(job / "receipt.json"), result)
                if excessive is None or cap is None:
                    self.assertEqual(result["status"], "succeeded", result)
                    published = service.root / "generations" / result["job_id"]
                    for name in ("asset.lmesh", "asset.lod1.lmesh", "asset.lod2.lmesh"):
                        self.assertEqual((published / name).read_bytes(), triangle(65 if name == excessive else 64))
                    if cap is None:
                        self.assertNotIn("settings", result)
                        self.assertNotIn("settings", service.generation(result["job_id"]))
                    generation = published
                    retained = {path.name: path.read_bytes() for path in generation.iterdir()}
                    pointer = (service.root / "current.json").read_bytes()
                else:
                    self.assertEqual(result["status"], "failed", result)
                    self.assertEqual(result["findings"][0]["rule_id"], "output.lod_exceeded")
                    self.assertEqual(result["findings"][0]["field"], excessive)
                    self.assertEqual((service.root / "current.json").read_bytes(), pointer)
                    self.assertFalse((service.root / "generations" / result["job_id"]).exists())
                    self.assertFalse((job / "outputs").exists())
                    self.assertEqual((job / "source.glb").read_bytes(), (self.project / "triangle.glb").read_bytes())
                    self.assertEqual({path.name for path in (job / "inputs").iterdir()},
                                     set(result["input_hashes"].values()))
                    for name, expected in result["input_hashes"].items():
                        self.assertEqual((job / "inputs" / expected).read_bytes(), (self.project / name).read_bytes())
                self.assertEqual({path.name: path.read_bytes() for path in generation.iterdir()}, retained)
        self.assertEqual(service.generation(previous["job_id"])["outputs"], previous["outputs"])

    def test_prefab_max_tris_refused_before_compilation(self):
        service = self.open()
        self.success(service)
        pointer = (service.root / "current.json").read_bytes()
        self.asset.update(profile=PREFAB_PROFILE, settings={"max_tris": 64})
        atomic_json(self.project / "asset.json", self.asset)
        with self.assertRaises(Refusal) as caught:
            service.validate("asset.json")
        self.assertEqual(caught.exception.finding["rule_id"], "prefab.max_tris")
        with patch.object(service, "_compile") as compiler, \
                patch.object(service, "_compile_prefab") as prefab_compiler:
            result = self.wait(service, service.submit("asset.json", 1))
        self.assertEqual(result["status"], "failed", result)
        self.assertEqual(result["findings"][0]["rule_id"], "prefab.max_tris")
        self.assertEqual(result["findings"][0]["field"], "settings.max_tris")
        compiler.assert_not_called()
        prefab_compiler.assert_not_called()
        self.assertEqual((service.root / "current.json").read_bytes(), pointer)

    def test_max_tris_capability_and_legacy_absence(self):
        service = self.open()
        self.assertIs(service.capabilities()["features"]["max_tris_setting"], True)
        features = dispatch(service, "capabilities", {})["features"]
        self.assertIs(features["max_tris_setting"], True)
        self.assertTrue(features["meshgen_script_pins"])
        self.assertTrue(features["meshgen_configuration"])
        class LegacyService:
            def capabilities(self):
                return {"schemas": [ASSET], "profiles": [PROFILE]}
        # Dispatch must not invent support for a service that lacks the field.
        legacy = dispatch(LegacyService(), "capabilities", {})
        self.assertFalse(legacy.get("features", {}).get("max_tris_setting", False))

    def test_producer_revision_guard_rejects_edits_before_extraction(self):
        service = self.open()
        self.success(service)
        pointer = service.current()
        self.asset["expected_dependency_hashes"] = {
            "source.blend": digest((self.project / "source.blend").read_bytes())}
        atomic_json(self.project / "asset.json", self.asset)
        (self.project / "source.blend").write_bytes(b"edit made before submission")
        with patch.object(service, "_compile") as compiler:
            result = self.wait(service, service.submit("asset.json", 1))
        self.assertEqual(result["status"], "stale")
        self.assertEqual(service.current(), pointer)
        compiler.assert_not_called()

    def test_partial_and_malformed_outputs_preserve_previous(self):
        service = self.open()
        previous = self.success(service)
        pointer = service.current()
        for payload in (b"", b"LMSH", triangle()[:-1], triangle() + b"extra"):
            def compile_bad(job_id, source, outputs, emit_lods, max_tris=None):
                (outputs / "asset.lmesh").write_bytes(payload)
            with patch.object(service, "_compile", side_effect=compile_bad):
                result = self.wait(service, service.submit("asset.json", 1))
            self.assertEqual(result["status"], "failed")
            self.assertEqual(service.current(), pointer)
            self.assertEqual((service.root / "jobs" / result["job_id"] / "outputs" / "asset.lmesh").read_bytes(), payload)
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
            def corrupt(job_id, source, outputs, emit_lods, max_tris=None):
                self.compile_valid(job_id, source, outputs, emit_lods, max_tris)
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
        capabilities = json.loads(result.stdout)["result"]
        self.assertTrue(capabilities["engine_compilation"])
        self.assertIs(capabilities["features"]["max_tris_setting"], True)
        for settings in (None, {"max_tris": 64}, {"max_tris": 1_000_000}, {"max_tris": True}):
            document = dict(self.asset)
            if settings is not None:
                document["settings"] = settings
            atomic_json(self.project / "asset.json", document)
            result = subprocess.run(command + ["validate", "asset.json"], cwd=self.root, env=env,
                                    capture_output=True, text=True, timeout=10)
            with self.subTest(settings=settings):
                self.assertEqual(result.returncode, 1 if settings == {"max_tris": True} else 0, result.stderr)
                if result.returncode:
                    self.assertEqual(json.loads(result.stdout)["findings"][0]["rule_id"], "settings.invalid")
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
        self.assertIs(rows[1]["result"]["features"]["max_tris_setting"], True)
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
