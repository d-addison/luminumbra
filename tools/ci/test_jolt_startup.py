#!/usr/bin/env python3
"""Compiler-free checks for the exact, read-only Jolt patch integration."""

import copy
import difflib
import hashlib
import importlib.util
import json
import os
import re
import shlex
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("jolt_startup", ROOT / "tools/ci/jolt_startup.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class PatchIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="jolt-patch-")
        self.addCleanup(self.temporary.cleanup)
        # Bind fixtures and fault injections to the paths prepare() accesses.
        # Retain TemporaryDirectory's original spelling for its own cleanup.
        self.base = Path(self.temporary.name).resolve()
        self.root = self.base / "project"
        self.source = self.base / "supplied"
        self.output = self.base / "build/overlay"
        self.name = "Jolt/Core/Memory.h"
        self.before = b"first\nold\nlast\n"
        self.after = b"first\nnew\nlast\n"
        self.patch = "".join(difflib.unified_diff(
            self.before.decode().splitlines(True), self.after.decode().splitlines(True),
            fromfile="a/" + self.name, tofile="b/" + self.name)).encode()
        self.manifest = {
            "version": 1, "revision": MODULE.REVISION, "tag": "v5.3.0",
            "repository": "https://github.com/jrouwe/JoltPhysics.git",
            "patch": "jolt-startup.patch", "sha256": MODULE.digest(self.patch),
            "files": [{"path": self.name, "before": MODULE.digest(self.before),
                       "after": MODULE.digest(self.after), "mode": "100644"}],
        }
        (self.source / self.name).parent.mkdir(parents=True)
        (self.source / self.name).write_bytes(self.before)
        (self.root / MODULE.MANIFEST).parent.mkdir(parents=True)
        self.save()

    def save(self):
        (self.root / MODULE.MANIFEST).write_text(json.dumps(self.manifest))
        (self.root / MODULE.MANIFEST.parent / "jolt-startup.patch").write_bytes(self.patch)

    def prepare(self):
        MODULE.prepare(self.source, self.output, self.root)

    def test_registered_contract_has_interpreter_labels_and_bounded_deadline(self):
        source = (ROOT / "test/CMakeLists.txt").read_text(encoding="utf-8")
        self.assertEqual(source.count("NAME JoltStartupPatchContract"), 1)
        # Execute the production registration alone: no native targets, compiler
        # probes, dependency configuration or CTest execution.
        registration = re.search(
            r"find_package\(Python3 COMPONENTS Interpreter REQUIRED\)\s+"
            r"add_test\(NAME JoltStartupPatchContract[^)]*\)\s+"
            r"set_tests_properties\(JoltStartupPatchContract[^)]*\)", source)
        self.assertIsNotNone(registration)
        project = self.base / "registration with spaces"
        project.mkdir()
        (project / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(JoltStartupRegistration LANGUAGES NONE)\n"
            "enable_testing()\n"
            f'set(CMAKE_SOURCE_DIR "{ROOT.as_posix()}")\n'
            + registration.group(0) + "\n", encoding="utf-8")
        build = project / "build"
        subprocess.run(
            ["cmake", "-S", str(project), "-B", str(build), "-G", "Ninja",
             f"-DPython3_EXECUTABLE={sys.executable}"],
            check=True, capture_output=True, text=True, timeout=10)
        result = subprocess.run(
            ["ctest", "--test-dir", str(build), "--show-only=json-v1"],
            check=True, capture_output=True, text=True, timeout=10)
        discovery = json.loads(result.stdout)
        self.assertEqual(len(discovery["tests"]), 1)
        test = discovery["tests"][0]
        self.assertEqual(test["name"], "JoltStartupPatchContract")
        self.assertEqual(len(test["command"]), 4)
        self.assertEqual(Path(test["command"][0]).resolve(), Path(sys.executable).resolve())
        self.assertEqual(test["command"][1:3], ["-I", "-B"])
        self.assertEqual(Path(test["command"][3]).resolve(), ROOT / "tools/ci/test_jolt_startup.py")
        properties = {item["name"]: item["value"] for item in test["properties"]}
        self.assertEqual(properties["LABELS"], ["ci", "tools"])
        self.assertEqual(properties["TIMEOUT"], 30)
        self.assertFalse(properties.get("DISABLED", False))
        self.assertNotIn("SKIP_RETURN_CODE", properties)
        self.assertNotIn("RUN_SERIAL", properties)
        self.assertNotRegex(
            (build / "CMakeCache.txt").read_text(encoding="utf-8"),
            r"(?m)^CMAKE_(?:C|CXX)_COMPILER:")

    def test_real_roster_and_all_input_hashes_are_bound(self):
        manifest, payload = MODULE.inputs()
        self.assertEqual(manifest["revision"], MODULE.REVISION)
        self.assertEqual(MODULE.digest(payload), manifest["sha256"])
        identity = MODULE.identity()
        for path, expected in identity["inputs"].items():
            self.assertEqual(hashlib.sha256((ROOT / path).read_bytes()).hexdigest(), expected)
        self.assertIn("Jolt/Core/LockFreeHashMap.h", [entry["path"] for entry in manifest["files"]])

    def test_default_pool_constructor_is_defined_only_in_library(self):
        _, payload = MODULE.inputs()
        edits = payload.decode("utf-8")
        header = edits.split("+++ b/Jolt/Core/JobSystemThreadPool.h\n", 1)[1].split("--- a/", 1)[0]
        implementation = edits.split("+++ b/Jolt/Core/JobSystemThreadPool.cpp\n", 1)[1].split("--- a/", 1)[0]
        self.assertRegex(header, r"(?m)^-\s+JobSystemThreadPool\(\) = default;$")
        self.assertRegex(header, r"(?m)^\+\s+JobSystemThreadPool\(\);$")
        self.assertEqual(implementation.count("+JobSystemThreadPool::JobSystemThreadPool() = default;"), 1)

    def test_offline_lock_binds_overlay_and_refuses_changed_integration(self):
        spec = importlib.util.spec_from_file_location("offline_deps", ROOT / "tools/ci/offline_deps.py")
        offline = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(offline)
        (self.root / "vendor").mkdir()
        (self.root / "vendor/CMakeLists.txt").write_text(
            "FetchContent_Declare(JoltPhysics GIT_REPOSITORY "
            "https://github.com/jrouwe/JoltPhysics.git GIT_TAG v5.3.0 SOURCE_SUBDIR Build)\n")
        # Exercise the real overlay declaration with its real manifest and code.
        for name in ("tools/ci/jolt_startup.py", "tools/ci/path_safety.py", "cmake/jolt_startup.cmake",
                     "cmake/patches/jolt-startup.json", "cmake/patches/jolt-startup.patch"):
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / name, target)
        lock = offline.unresolved_lock(self.root)
        entry = lock["dependencies"]["joltphysics"]
        self.assertEqual(entry["overlay"], MODULE.identity(self.root))
        self.assertEqual(entry["revision"], "v5.3.0")
        self.assertEqual(lock["status"], "unresolved")
        self.assertIsNone(entry["commit"])
        self.assertIsNone(entry["sha256"])
        original = copy.deepcopy(entry["overlay"])
        path = self.root / "cmake/jolt_startup.cmake"
        path.write_bytes(path.read_bytes() + b"\n# changed integration\n")
        self.assertNotEqual(offline.declarations(self.root)["joltphysics"]["overlay"], original)
        lock["status"] = "resolved"
        lock_path = self.base / "lock.json"
        lock_path.write_text(json.dumps(lock))
        with self.assertRaisesRegex(offline.DependencyError, "wrong declared dependency"):
            offline.verify(self.root, self.base / "absent-cache", lock_path)
        (self.root / "tools/ci/jolt_startup.py").unlink()
        with self.assertRaisesRegex(offline.DependencyError, "overlay identity"):
            offline.declarations(self.root)

    def copy_integration(self, destination):
        for name in MODULE.identity()["inputs"]:
            target = destination / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes((ROOT / name).read_bytes())

    def test_autocrlf_checkout_preserves_actual_input_bytes_identity_and_overlay(self):
        project = self.base / "private checkout"
        project.mkdir()
        self.copy_integration(project)
        (project / ".gitattributes").write_bytes((ROOT / ".gitattributes").read_bytes())
        # A control proves Git actually converts unspecified text in this fixture.
        (project / "conversion-control.txt").write_bytes(b"first\nsecond\n")

        def git(*arguments):
            return subprocess.run(
                ["git", "-C", str(project), *arguments], check=True,
                capture_output=True, text=True, timeout=10)

        git("init")
        git("config", "core.autocrlf", "true")
        git("config", "user.name", "Fixture")
        git("config", "user.email", "fixture@example.invalid")
        git("config", "commit.gpgsign", "false")
        hooks = self.base / "empty hooks"
        hooks.mkdir()
        git("config", "core.hooksPath", str(hooks))
        git("add", ".")
        git("commit", "-m", "Checkout fixture")
        for name in [*MODULE.identity()["inputs"], ".gitattributes", "conversion-control.txt"]:
            (project / name).unlink()
        git("checkout", "HEAD", "--", ".")
        self.assertEqual((project / "conversion-control.txt").read_bytes(), b"first\r\nsecond\r\n")
        spec = importlib.util.spec_from_file_location("checkout_overlay", project / "tools/ci/jolt_startup.py")
        checked_out = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(checked_out)
        expected = MODULE.identity()
        self.assertEqual(checked_out.identity(), expected)
        self.assertEqual(len(expected["inputs"]), 6)
        for name in expected["inputs"]:
            self.assertEqual((project / name).read_bytes(), (ROOT / name).read_bytes())
            attributes = git("check-attr", "text", "eol", "--", name).stdout.splitlines()
            self.assertEqual([line.rsplit(": ", 1)[-1] for line in attributes], ["set", "lf"])
        # Execute the materialized verifier on a small complete patch fixture.
        # Actual upstream eighteen-file qualification is separate from this test.
        checked_out.prepare(self.source, self.output, self.root)
        self.assertEqual((self.output / self.name).read_bytes(), self.after)
        self.assertEqual((self.source / self.name).read_bytes(), self.before)

    def test_configure_requires_supported_python_with_testing_off(self):
        self.copy_integration(self.root)
        self.save()
        (self.root / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(JoltOverlayContract LANGUAGES NONE)\n"
            "option(BUILD_TESTING \"Testing\" OFF)\n"
            "if(BUILD_TESTING)\n  message(FATAL_ERROR \"Testing must be off\")\nendif()\n"
            "add_library(Jolt OBJECT EXCLUDE_FROM_ALL)\n"
            "set_target_properties(Jolt PROPERTIES LINKER_LANGUAGE CXX)\n"
            f'set_property(TARGET Jolt PROPERTY SOURCES "{(self.source / self.name).as_posix()}")\n'
            'include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/jolt_startup.cmake")\n'
            f'luminumbra_prepare_jolt_startup("{self.source.as_posix()}" "{self.output.as_posix()}")\n'
            'get_target_property(_sources Jolt SOURCES)\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/sources.txt" "${_sources}")\n', encoding="utf-8")
        unsupported = self.base / ("unsupported.cmd" if os.name == "nt" else "unsupported")
        driver = self.base / "interpreter-metadata.py"
        driver.write_text(
            "import os, sys\n"
            "if len(sys.argv) == 3 and sys.argv[1] == '-c':\n"
            "    sys.version_info = (3, 9, 0, 'final', 0)\n    exec(sys.argv[2])\n"
            "else:\n    os.execv(sys.executable, [sys.executable, *sys.argv[1:]])\n", encoding="utf-8")
        command = subprocess.list2cmdline([sys.executable, "-I", "-B", str(driver)])
        unsupported.write_text(
            "@echo off\n" + command + " %*\n" if os.name == "nt" else
            "#!/bin/sh\nexec " + shlex.join([sys.executable, "-I", "-B", str(driver)]) + ' "$@"\n')
        unsupported.chmod(0o755)
        # This executable supplies only old interpreter version metadata to the
        # real FindPython3 probe; no substitute verifier or package finder runs.
        for label, interpreter in (("missing", self.base / "absent-python"),
                                   ("unsupported", unsupported), ("supported", Path(sys.executable))):
            with self.subTest(interpreter=label):
                build = self.base / ("configure-" + label)
                result = subprocess.run(
                    ["cmake", "-S", str(self.root), "-B", str(build), "-G", "Ninja",
                     "-DBUILD_TESTING=OFF", f"-DPython3_EXECUTABLE={interpreter}"],
                    capture_output=True, text=True, timeout=10)
                if label == "supported":
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual((self.output / self.name).read_bytes(), self.after)
                    self.assertEqual((build / "sources.txt").read_text(), (self.output / self.name).as_posix())
                else:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("Python3", result.stdout + result.stderr)
                    self.assertIn("3.10", result.stdout + result.stderr)
                    if label == "unsupported":
                        self.assertIn("unsuitable version", result.stdout + result.stderr)
                    self.assertFalse(self.output.exists())
                self.assertEqual((self.source / self.name).read_bytes(), self.before)
                self.assertNotRegex((build / "CMakeCache.txt").read_text(), r"(?m)^CMAKE_(?:C|CXX)_COMPILER:")

    def test_fixture_aliases_bind_injections_to_accessed_paths(self):
        temporary_directory = tempfile.TemporaryDirectory
        spellings = []

        def aliased_temporary(*args, **kwargs):
            temporary = temporary_directory(*args, **kwargs)
            original = Path(temporary.name)
            # A real existing directory gives a portable spelling alias. This
            # does not assume Windows short-name support or symlink privilege.
            (original / "spelling").mkdir()
            alias = original / "spelling/.."
            self.assertNotEqual(alias, alias.resolve())
            self.assertTrue(alias.samefile(original))
            spellings.append((alias, original.resolve()))

            class TemporaryAlias:
                name = str(alias)
                cleanup = temporary.cleanup

            return TemporaryAlias()

        methods = ("test_configure_requires_supported_python_with_testing_off",
                   "test_resolved_output_alias_cannot_enter_supplied_source",
                   "test_short_write_reports_failure_and_preserves_partial_output",
                   "test_fifo_directory_and_redirect_metadata_refuse")
        for method in methods:
            with self.subTest(method=method), patch.object(tempfile, "TemporaryDirectory", aliased_temporary):
                result = unittest.TestResult()
                PatchIntegrationTests(method).run(result)
                self.assertEqual(result.testsRun, 1)
                self.assertEqual(result.skipped, [])
                self.assertEqual(result.errors + result.failures, [])
        self.assertEqual(len(spellings), len(methods))

    def test_cmake_source_spellings_match_and_missing_or_duplicate_inputs_refuse(self):
        self.copy_integration(self.root)
        self.save()
        (self.source / "Build").mkdir()
        target = self.source / self.name
        source_alias = self.source / "Build/.."
        self.assertTrue(source_alias.samefile(self.source))
        cases = (("source-spelling", source_alias, [target.as_posix()], None),
                 ("relative-source", self.source, ["../" + self.name], None),
                 ("duplicate", self.source, [target.as_posix(), "../" + self.name], "Duplicate Jolt input"),
                 ("missing", self.source, [(self.source / "absent.h").as_posix()], "missing reviewed input"),
                 ("empty", self.source, [], "missing reviewed input"))
        for label, source, sources, refusal in cases:
            with self.subTest(case=label):
                output = self.base / ("overlay-" + label)
                (self.root / "CMakeLists.txt").write_text(
                    "cmake_minimum_required(VERSION 3.20)\n"
                    "project(JoltSourcePaths LANGUAGES NONE)\n"
                    "add_library(Jolt OBJECT EXCLUDE_FROM_ALL)\n"
                    "set_target_properties(Jolt PROPERTIES LINKER_LANGUAGE CXX)\n"
                    f'set_property(TARGET Jolt PROPERTY SOURCES "{";".join(sources)}")\n'
                    'include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/jolt_startup.cmake")\n'
                    f'luminumbra_prepare_jolt_startup("{source.as_posix()}" "{output.as_posix()}")\n'
                    'get_target_property(_sources Jolt SOURCES)\n'
                    'file(WRITE "${CMAKE_BINARY_DIR}/sources.txt" "${_sources}")\n', encoding="utf-8")
                build = self.base / ("source-paths-" + label)
                result = subprocess.run(
                    ["cmake", "-S", str(self.root), "-B", str(build), "-G", "Ninja",
                     f"-DPython3_EXECUTABLE={sys.executable}"],
                    capture_output=True, text=True, timeout=10)
                if refusal is None:
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual((build / "sources.txt").read_text(), (output / self.name).as_posix())
                    self.assertEqual((output / self.name).read_bytes(), self.after)
                else:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(refusal, result.stdout + result.stderr)
                self.assertEqual(target.read_bytes(), self.before)
                self.assertNotRegex((build / "CMakeCache.txt").read_text(), r"(?m)^CMAKE_(?:C|CXX)_COMPILER:")

    def test_uniform_crlf_preimages_preserve_source_and_produce_exact_lf(self):
        before = b"\tfirst \t\n\nold\nlast \t\n"
        after = before.replace(b"old\n", b"new\n")
        payload = "".join(difflib.unified_diff(
            before.decode().splitlines(True), after.decode().splitlines(True),
            fromfile="a/" + self.name, tofile="b/" + self.name)).encode()
        self.install_edit(before, after, payload)
        target = self.source / self.name
        encoded = before.replace(b"\n", b"\r\n")
        target.write_bytes(encoded)
        target.chmod(0o444)
        metadata = target.lstat()
        self.prepare()
        output_metadata = (self.output / self.name).lstat()
        self.prepare()
        self.assertEqual(target.read_bytes(), encoded)
        self.assertEqual((self.output / self.name).read_bytes(), after)
        self.assertNotIn(b"\r", (self.output / self.name).read_bytes())
        for path, previous in ((target, metadata), (self.output / self.name, output_metadata)):
            current = path.lstat()
            for field in ("st_mode", "st_size", "st_mtime_ns", "st_ctime_ns", "st_ino", "st_dev", "st_nlink"):
                self.assertEqual(getattr(current, field), getattr(previous, field), field)

    def test_crlf_cli_and_cmake_consumers_repeat_and_refuse_corruption(self):
        self.copy_integration(self.root)
        self.save()
        target = self.source / self.name
        encoded = self.before.replace(b"\n", b"\r\n")
        target.write_bytes(encoded)
        metadata = target.lstat()
        (self.root / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(JoltTextCheckout LANGUAGES NONE)\n"
            "add_library(Jolt OBJECT EXCLUDE_FROM_ALL)\n"
            "set_target_properties(Jolt PROPERTIES LINKER_LANGUAGE CXX)\n"
            f'set_property(TARGET Jolt PROPERTY SOURCES "{target.as_posix()}")\n'
            'include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/jolt_startup.cmake")\n'
            f'luminumbra_prepare_jolt_startup("{self.source.as_posix()}" "{self.output.as_posix()}")\n'
            'get_target_property(_sources Jolt SOURCES)\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/sources.txt" "${_sources}")\n', encoding="utf-8")
        build = self.base / "text-checkout-build"
        commands = ([sys.executable, "-I", "-B", str(self.root / "tools/ci/jolt_startup.py"),
                     "--source", str(self.source), "--output", str(self.output)],
                    ["cmake", "-S", str(self.root), "-B", str(build), "-G", "Ninja",
                     f"-DPython3_EXECUTABLE={sys.executable}"])
        for command in commands:
            for repeat in range(2):
                with self.subTest(command=command[0], repeat=repeat):
                    result = subprocess.run(command, capture_output=True, text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual((self.output / self.name).read_bytes(), self.after)
                    self.assertEqual(target.read_bytes(), encoded)
                    current = target.lstat()
                    for field in ("st_mode", "st_size", "st_mtime_ns", "st_ctime_ns", "st_ino", "st_dev", "st_nlink"):
                        self.assertEqual(getattr(current, field), getattr(metadata, field), field)
        self.assertEqual((build / "sources.txt").read_text(), (self.output / self.name).as_posix())
        self.assertNotRegex((build / "CMakeCache.txt").read_text(), r"(?m)^CMAKE_(?:C|CXX)_COMPILER:")
        corrupted = encoded.replace(b"old", b"bad")
        target.write_bytes(corrupted)
        output_metadata = (self.output / self.name).lstat()
        for command in commands:
            result = subprocess.run(command, capture_output=True, text=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertRegex(result.stdout + result.stderr, r"wrong pinned\s+preimage")
            self.assertEqual(target.read_bytes(), corrupted)
            self.assertEqual((self.output / self.name).read_bytes(), self.after)
            self.assertEqual((self.output / self.name).lstat().st_mtime_ns, output_metadata.st_mtime_ns)

    def test_corrupted_crlf_mixed_and_lone_cr_preimages_refuse_untouched(self):
        encoded = self.before.replace(b"\n", b"\r\n")
        malformed = (encoded.replace(b"old", b"bad"), b"\xef\xbb\xbf" + encoded,
                     encoded.replace(b"old", b"old "), encoded.replace(b"\r\n", b"\n", 1),
                     self.before.replace(b"\n", b"\r\n", 1), encoded.replace(b"\r\n", b"\r", 1),
                     encoded.replace(b"old", b"o\rld"), encoded.replace(b"\r\n", b"\r\r\n"),
                     encoded[:-1], encoded[:-2], encoded + b"\r", encoded.replace(b"old", b"\xffld"), b"")
        target = self.source / self.name
        for materialized in (False, True):
            self.output = self.base / ("materialized" if materialized else "absent")
            if materialized:
                target.write_bytes(self.before)
                self.prepare()
            for content in malformed:
                with self.subTest(materialized=materialized, content=content):
                    target.write_bytes(content)
                    metadata = target.lstat()
                    previous = (self.output / self.name).lstat() if materialized else None
                    with self.assertRaises(MODULE.PatchError):
                        self.prepare()
                    self.assertEqual(target.read_bytes(), content)
                    self.assertEqual(target.lstat().st_mtime_ns, metadata.st_mtime_ns)
                    if materialized:
                        self.assertEqual((self.output / self.name).read_bytes(), self.after)
                        self.assertEqual((self.output / self.name).lstat().st_mtime_ns, previous.st_mtime_ns)
                    else:
                        self.assertFalse(self.output.exists())

    def test_crlf_raw_file_and_total_size_bounds_precede_conversion(self):
        target = self.source / self.name
        encoded = self.before.replace(b"\n", b"\r\n")
        target.write_bytes(encoded)
        with patch.object(MODULE, "MAX_BYTES", len(self.before)), \
                patch.object(MODULE, "inputs", return_value=(self.manifest, self.patch)):
            with self.assertRaisesRegex(MODULE.PatchError, "regular-file bound"):
                self.prepare()
        second = "Jolt/Core/Other.h"
        (self.source / second).write_bytes(encoded)
        manifest = copy.deepcopy(self.manifest)
        manifest["files"].append({**manifest["files"][0], "path": second})
        with patch.object(MODULE, "MAX_BYTES", len(encoded)), \
                patch.object(MODULE, "inputs", return_value=(manifest, self.patch)):
            with self.assertRaisesRegex(MODULE.PatchError, "edit payload exceeds bound"):
                self.prepare()
        self.assertFalse(self.output.exists())
        self.assertEqual(target.read_bytes(), encoded)
        self.assertEqual((self.source / second).read_bytes(), encoded)

    def test_repeat_is_idempotent_and_source_is_read_only(self):
        self.prepare()
        target = self.output / self.name
        metadata = target.stat()
        self.prepare()
        self.assertEqual(target.read_bytes(), self.after)
        self.assertEqual(target.stat().st_mtime_ns, metadata.st_mtime_ns)
        self.assertEqual((self.source / self.name).read_bytes(), self.before)
        self.assertEqual(list(self.source.rglob("*")),
                         [self.source / "Jolt", self.source / "Jolt/Core", self.source / self.name])

    def test_missing_mismatched_or_partial_preimage_refuses_before_writes(self):
        for content in (None, b"first\n", b"first\nforeign\nlast\n"):
            with self.subTest(content=content):
                target = self.source / self.name
                if content is None:
                    target.unlink()
                else:
                    target.write_bytes(content)
                with self.assertRaises((MODULE.PatchError, FileNotFoundError)):
                    self.prepare()
                self.assertFalse(self.output.exists())
                if content is None:
                    target.write_bytes(self.before)

    def test_revision_patch_digest_and_output_digest_refuse(self):
        original = copy.deepcopy(self.manifest)
        for key, value in (("revision", "0" * 40), ("tag", "latest"), ("sha256", "0" * 64)):
            with self.subTest(key=key):
                self.manifest = copy.deepcopy(original)
                self.manifest[key] = value
                self.save()
                with self.assertRaises(MODULE.PatchError):
                    self.prepare()
                self.assertFalse(self.output.exists())
        self.manifest = copy.deepcopy(original)
        self.manifest["files"][0]["after"] = "0" * 64
        self.save()
        with self.assertRaisesRegex(MODULE.PatchError, "output"):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_context_and_hunk_counts_refuse_even_with_matching_patch_digest(self):
        original = self.patch
        for payload in (original.replace(b"-old\n", b"-absent\n"),
                        original.replace(b"@@ -1,3 +1,3 @@", b"@@ -1,4 +1,3 @@"),
                        original.replace(b"+++ b/", b"+++ b/other/"),
                        original.replace(b"@@ -1,3 +1,3 @@", b"@@ -2,3 +1,3 @@")):
            with self.subTest(payload=payload):
                self.patch = payload
                self.manifest["sha256"] = MODULE.digest(payload)
                self.save()
                with self.assertRaises(MODULE.PatchError):
                    self.prepare()
                self.assertFalse(self.output.exists())

    def install_edit(self, before, after, payload):
        self.before, self.after, self.patch = before, after, payload
        (self.source / self.name).write_bytes(before)
        self.manifest["sha256"] = MODULE.digest(payload)
        self.manifest["files"][0].update(before=MODULE.digest(before), after=MODULE.digest(after))
        self.save()

    def test_zero_context_insertions_at_start_middle_end_and_empty_source(self):
        for index, (before, position) in enumerate(((b"", 0), (self.before, 0),
                                                     (self.before, 1), (self.before, 3))):
            with self.subTest(position=position, before=before):
                lines = before.splitlines(True)
                after = b"".join(lines[:position]) + b"\tinserted\n\n" + b"".join(lines[position:])
                payload = "".join(difflib.unified_diff(
                    before.decode().splitlines(True), after.decode().splitlines(True),
                    fromfile="a/" + self.name, tofile="b/" + self.name, n=0)).encode()
                self.assertIn(f"@@ -{position},0 +{position + 1},2 @@\n".encode(), payload)
                self.install_edit(before, after, payload)
                self.output = self.base / f"build/insertion-{index}"
                self.prepare()
                self.assertEqual((self.output / self.name).read_bytes(), after)
                self.assertEqual((self.source / self.name).read_bytes(), before)
                self.prepare()
                self.assertEqual((self.output / self.name).read_bytes(), after)

    def test_zero_context_deletions_at_start_middle_end_and_entire_source(self):
        before = self.before
        for index, (start, count) in enumerate(((0, 1), (1, 1), (2, 1), (0, 3))):
            with self.subTest(start=start, count=count):
                lines = before.splitlines(True)
                after = b"".join(lines[:start] + lines[start + count:])
                payload = "".join(difflib.unified_diff(
                    before.decode().splitlines(True), after.decode().splitlines(True),
                    fromfile="a/" + self.name, tofile="b/" + self.name, n=0)).encode()
                self.assertIn(f" +{start},0 @@\n".encode(), payload)
                self.install_edit(before, after, payload)
                self.output = self.base / f"build/deletion-{index}"
                self.prepare()
                self.assertEqual((self.output / self.name).read_bytes(), after)
                self.assertEqual((self.source / self.name).read_bytes(), before)

    def test_zero_and_context_backed_replacements_preserve_exact_bytes(self):
        before = b"\tfirst\n\nold\nkeep-a\nkeep-b\nkeep-c\nkeep-d\nkeep-e\nlast\n"
        after = before.replace(b"old\n", b"\tnew\n\n").replace(b"last\n", b"final\n")
        for context in (0, 3):
            with self.subTest(context=context):
                payload = "".join(difflib.unified_diff(
                    before.decode().splitlines(True), after.decode().splitlines(True),
                    fromfile="a/" + self.name, tofile="b/" + self.name, n=context)).encode()
                self.install_edit(before, after, payload)
                self.output = self.base / f"build/replacement-{context}"
                self.prepare()
                self.assertEqual((self.output / self.name).read_bytes(), after)
                self.assertEqual((self.source / self.name).read_bytes(), before)
        # Omitted counts mean one, as in the supported context patch format.
        payload = (f"--- a/{self.name}\n+++ b/{self.name}\n"
                   "@@ -2 +2 @@\n-old\n+new\n").encode()
        self.install_edit(b"first\nold\nlast\n", b"first\nnew\nlast\n", payload)
        self.output = self.base / "build/implicit-counts"
        self.prepare()
        self.assertEqual((self.output / self.name).read_bytes(), self.after)

    def test_zero_context_malformed_ranges_offsets_counts_and_overlap_refuse(self):
        header = f"--- a/{self.name}\n+++ b/{self.name}\n".encode()
        hunks = (
            b"@@ -4,0 +5 @@\n+inserted\n",  # Beyond the old end.
            b"@@ -0,0 +0 @@\n+inserted\n",  # Nonempty new start is one-based.
            b"@@ -1,0 +1 @@\n+inserted\n",  # Insertion output offset is wrong.
            b"@@ -0 +0,0 @@\n-first\n",    # Nonempty old start is one-based.
            b"@@ -1 +1,0 @@\n-first\n",    # Deletion output boundary is wrong.
            b"@@ -1,0 +2,2 @@\n+inserted\n",  # Partial insertion.
            b"@@ -1,0 +2 @@\n+one\n+two\n",  # Too many new records.
            b"@@ -1,0 +2 @@\n-first\n+inserted\n",  # Zero old count consumes input.
            b"@@ -1 +0,0 @@\n-first\n+inserted\n",  # Zero new count produces output.
            b"@@ -1,2 +0,0 @@\n-first\n",   # Partial deletion.
            b"@@ -1 +0,0 @@\n-first\n-old\n",  # Too many old records.
            b"@@ -1 +0,0 @@\n-absent\n",   # Deletion content must match.
            b"@@ -1 +0,0 @@\n-first\n@@ -0,0 +1 @@\n+inserted\n",  # Backwards insertion.
            b"@@ -2 +1,0 @@\n-old\n@@ -2 +1,0 @@\n-old\n",  # Overlapping deletions.
            b"@@ -2 +2 @@\n-old\n+new\n@@ -2 +3 @@\n-old\n+again\n",  # Overlapping replacements.
            b"@@ -1,0 +2 @@\n",  # Missing insertion payload.
            b"@@ -1 +0,0 @@\n",  # Missing deletion payload.
            b"@@ -1,0 +2 @@\n+inserted\n? metadata\n",  # Unknown record.
            b"@@ -1,0 +2 @@\n+inserted\n\n",  # Unprefixed blank record.
            b"@@ -1,0 +2 @@\n+inserted\n\\ No newline at end of file\n",  # Unsupported metadata.
            b"@@ -1,0 +2 @@",  # Partial header.
            b"@@ --1,0 +2 @@\n+inserted\n",
            b"@@ -1,0 +2,-1 @@\n+inserted\n",
            b"@@ -1,0 +2 @@ extra\n+inserted\n",
        )
        before = self.before
        for index, hunk in enumerate(hunks):
            with self.subTest(hunk=hunk):
                self.install_edit(before, self.after, header + hunk)
                self.output = self.base / f"build/refusal-{index}"
                with self.assertRaises(MODULE.PatchError):
                    self.prepare()
                self.assertFalse(self.output.exists())
                self.assertEqual((self.source / self.name).read_bytes(), before)

    def test_reviewed_capacity_bounds_use_macro_safe_limits(self):
        _, payload = MODULE.inputs()
        expected = {
            b"Jolt/Core/HashTable.h":
                b"if (bytes > (std::numeric_limits<size_t>::max)() - 15)",
            b"Jolt/Core/JobSystemThreadPool.cpp":
                b"if (hardware_threads > uint((std::numeric_limits<int>::max)()))",
            b"Jolt/Core/Memory.h":
                b"if (inSize != 0 && inCount > (std::numeric_limits<size_t>::max)() / inSize)",
            b"Jolt/Physics/Collision/BroadPhase/BroadPhaseQuadTree.cpp":
                b"if (num_leaves_plus_internal_nodes > (uint64((std::numeric_limits<uint32>::max)()) - 1) / 2)",
        }
        additions = {}
        name = None
        for line in payload.splitlines():
            if line.startswith(b"+++ b/"):
                name = line[6:]
                additions[name] = []
            elif line.startswith(b"+"):
                additions[name].append(line[1:])
        for name, expression in expected.items():
            with self.subTest(path=name):
                self.assertIn(expression, b"\n".join(additions[name]))
        # Scan every added call, including bounds introduced in other files.
        for name, lines in additions.items():
            with self.subTest(path=name):
                self.assertNotRegex(
                    b"\n".join(lines),
                    rb"std::numeric_limits<[^>\n]+>::(?:min|max)\s*\(")

    def test_reviewed_patch_uses_zero_context_without_whitespace_errors(self):
        payload = (ROOT / MODULE.MANIFEST.parent / "jolt-startup.patch").read_bytes()
        lines = payload.splitlines()
        self.assertTrue(any(line.startswith(b"@@") for line in lines))
        self.assertFalse(any(line.startswith(b" ") for line in lines))
        self.assertTrue(all(line and line.rstrip(b" \t") == line for line in lines))

    def test_roster_escape_duplicate_and_missing_patch_file_refuse(self):
        original = copy.deepcopy(self.manifest)
        for name in ("Jolt/../outside.h", "/Jolt/Core/Memory.h", "Jolt/Core/name;write.h",
                     "Jolt/" + "a" * 513 + ".h"):
            self.manifest = copy.deepcopy(original)
            self.manifest["files"][0]["path"] = name
            self.save()
            with self.assertRaises(MODULE.PatchError):
                self.prepare()
            self.assertFalse(self.output.exists())
        self.manifest = copy.deepcopy(original)
        self.manifest["files"] *= 2
        self.save()
        with self.assertRaises(MODULE.PatchError):
            self.prepare()
        self.manifest = copy.deepcopy(original)
        self.save()
        (self.root / MODULE.MANIFEST.parent / "jolt-startup.patch").unlink()
        with self.assertRaises(FileNotFoundError):
            self.prepare()
        self.assertFalse(self.output.exists())

    def test_foreign_output_is_preserved_and_source_output_overlap_refuses(self):
        target = self.output / self.name
        target.parent.mkdir(parents=True)
        target.write_bytes(b"foreign")
        with self.assertRaises(MODULE.PatchError):
            self.prepare()
        self.assertEqual(target.read_bytes(), b"foreign")
        with self.assertRaises(MODULE.PatchError):
            MODULE.prepare(self.source, self.source / "generated", self.root)
        self.assertEqual((self.source / self.name).read_bytes(), self.before)

    def test_unlisted_overlay_headers_and_directories_refuse_before_writes(self):
        for index, name in enumerate(("Jolt/Jolt.h", "Jolt/extra", "foreign")):
            self.output = self.base / f"build/case-{index}"
            with self.subTest(name=name):
                target = self.output / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(b"foreign")
                with self.assertRaisesRegex(MODULE.PatchError, "unexpected"):
                    self.prepare()
                self.assertEqual(target.read_bytes(), b"foreign")
                self.assertFalse((self.output / self.name).exists())
                target.unlink()
        # Empty directories outside the roster also refuse.
        (self.output / "Jolt/extra").mkdir(parents=True)
        with self.assertRaisesRegex(MODULE.PatchError, "unexpected"):
            self.prepare()

    def test_resolved_output_alias_cannot_enter_supplied_source(self):
        original = Path.resolve
        alias = self.base / "short-name/overlay"
        injected = []

        def resolved(path, *args, **kwargs):
            if path == alias:
                injected.append(path)
                return self.source / "overlay"
            return original(path, *args, **kwargs)

        with patch.object(Path, "resolve", resolved):
            with self.assertRaisesRegex(MODULE.PatchError, "separate"):
                MODULE.prepare(self.source, alias, self.root)
        self.assertEqual(injected, [alias])
        self.assertFalse(alias.exists())
        self.assertFalse((self.source / "overlay").exists())

    def test_flush_failure_keeps_only_created_payload_and_repeat_never_overwrites(self):
        with patch.object(MODULE.os, "fsync", side_effect=OSError("flush refused")):
            with self.assertRaisesRegex(OSError, "flush refused"):
                self.prepare()
        target = self.output / self.name
        self.assertEqual(target.read_bytes(), self.after)
        self.assertEqual((self.source / self.name).read_bytes(), self.before)
        self.prepare()
        target.write_bytes(self.after[:2])
        with self.assertRaisesRegex(MODULE.PatchError, "content mismatch"):
            self.prepare()
        self.assertEqual(target.read_bytes(), self.after[:2])

    def test_short_write_reports_failure_and_preserves_partial_output(self):
        original = Path.open
        target = self.output / self.name
        injected = []

        class ShortWriter:
            def __init__(self, handle):
                self.handle = handle

            def __enter__(self):
                return self

            def __exit__(self, *args):
                return self.handle.__exit__(*args)

            def write(self, content):
                return self.handle.write(content[:2])

        def opened(path, *args, **kwargs):
            handle = original(path, *args, **kwargs)
            if path == target and args == ("xb",):
                injected.append(path)
                return ShortWriter(handle)
            return handle

        with patch.object(Path, "open", opened):
            with self.assertRaisesRegex(MODULE.PatchError, "short overlay write"):
                self.prepare()
        self.assertEqual(injected, [target])
        self.assertEqual(target.read_bytes(), self.after[:2])
        with self.assertRaisesRegex(MODULE.PatchError, "content mismatch"):
            self.prepare()
        self.assertEqual((self.source / self.name).read_bytes(), self.before)

    def test_parent_traversal_and_malformed_manifest_refuse(self):
        with self.assertRaisesRegex(MODULE.PatchError, "traversal"):
            MODULE.prepare(self.source, self.source / "../generated", self.root)
        for value in ([], {**self.manifest, "files": [None]},
                      {**self.manifest, "files": [{"path": 7}]}):
            with self.subTest(value=value):
                (self.root / MODULE.MANIFEST).write_text(json.dumps(value))
                with self.assertRaises(MODULE.PatchError):
                    self.prepare()
                self.assertFalse(self.output.exists())

    def test_fifo_directory_and_redirect_metadata_refuse(self):
        target = self.source / self.name
        target.unlink()
        target.mkdir()
        with self.assertRaisesRegex(MODULE.PatchError, "regular"):
            self.prepare()
        self.assertFalse(self.output.exists())
        target.rmdir()
        target.write_bytes(self.before)
        original = Path.lstat
        injected = []

        def redirected(path):
            info = original(path)
            if path == target:
                injected.append(path)
                return type("Reparse", (), {"st_mode": info.st_mode, "st_file_attributes": 0x400})()
            return info

        with patch.object(Path, "lstat", redirected):
            with self.assertRaisesRegex(MODULE.PatchError, "redirected"):
                self.prepare()
        self.assertTrue(injected)
        self.assertTrue(all(path == target for path in injected))
        self.assertEqual(target.read_bytes(), self.before)
        if hasattr(os, "mkfifo"):
            target.unlink()
            os.mkfifo(target)
            with self.assertRaisesRegex(MODULE.PatchError, "regular"):
                self.prepare()
        self.assertFalse(self.output.exists())

    def test_cli_missing_source_returns_failure_without_output(self):
        result = subprocess.run([sys.executable, "-I", "-B", str(ROOT / "tools/ci/jolt_startup.py"),
                                 "--source", str(self.base / "missing"), "--output", str(self.output)],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 1)
        self.assertIn("refused", result.stderr)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
