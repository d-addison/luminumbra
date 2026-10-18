#!/usr/bin/env python3
"""Exercise delivery using the complete immutable vendor input and real Git apply.

An offline author may supply a pristine archive extraction directly under TMPDIR
with LUMINUMBRA_RMLUI_TEST_SOURCE. It is temporarily moved, never copied or used
as a production source override. Otherwise the fixture downloads the exact pin.
"""
from __future__ import annotations

import concurrent.futures
import hashlib
import importlib.util
import json
import os
import re
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import textwrap
import unittest
from unittest import mock
import zlib

MODULE_PATH = Path(__file__).with_name("rmlui_source.py")
LOADER = importlib.util.spec_from_file_location("rmlui_source", MODULE_PATH)
assert LOADER and LOADER.loader
M = importlib.util.module_from_spec(LOADER)
LOADER.loader.exec_module(M)
ROOT = MODULE_PATH.resolve().parents[2]


def specification():
    return M.Specification(ROOT / "vendor/rmlui/rmlui-6.1-source.json",
                           ROOT / "cmake/patches/rmlui-6.1-shadow.patch")


def object_file(source, kind, data):
    raw = kind.encode() + b" " + str(len(data)).encode() + b"\0" + data
    identity = hashlib.sha1(raw).hexdigest()
    path = source / ".git/objects" / identity[:2] / identity[2:]
    path.parent.mkdir(exist_ok=True)
    if not path.exists():
        path.write_bytes(zlib.compress(raw))
    return identity


def install_exact_git(source, spec):
    """Reconstitute the original Git objects from the full verified archive."""
    spec.verify_files(source, False)
    M.git(source, "init", "--template=")
    directory = {}
    for name, entry in spec.files.items():
        data = M.read_file(source / name, entry["size"])
        identity = object_file(source, "blob", data)
        if identity != entry["blob"]:
            raise AssertionError(name)
        parts = name.split("/")
        node = directory
        for component in parts[:-1]:
            node = node.setdefault(component, {})
        node[parts[-1]] = identity

    def write_tree(node):
        records = []
        for name in sorted(node, key=lambda n: (n + ("/" if isinstance(node[n], dict) else "")).encode()):
            child = node[name]
            is_directory = isinstance(child, dict)
            identity = write_tree(child) if is_directory else child
            records.append((b"40000" if is_directory else b"100644") + b" " + name.encode() + b"\0" + bytes.fromhex(identity))
        return object_file(source, "tree", b"".join(records))

    if write_tree(directory) != spec.data["tree"]:
        raise AssertionError("tree identity")
    if object_file(source, "commit", spec.data["commit"].encode()) != M.REVISION:
        raise AssertionError("commit identity")
    (source / ".git/HEAD").write_bytes((M.REVISION + "\n").encode())
    (source / ".git/shallow").write_bytes((M.REVISION + "\n").encode())
    M.git(source, "read-tree", M.REVISION)
    M.verify_git(source, spec)


class SourceFiles(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.spec = specification()
        cls.directory = Path(tempfile.mkdtemp(prefix="rmlui-contract-")).resolve(strict=True)
        cls.workspace = M.Workspace(cls.directory / "build", cls.spec)
        cls.workspace.initialize()
        cls.offline = os.environ.get("LUMINUMBRA_RMLUI_TEST_SOURCE")
        if cls.offline:
            cls.input = M.canonical(cls.offline)
            if cls.input.parent != M.canonical(os.environ["TMPDIR"]):
                raise AssertionError("offline input must belong directly to the private temporary root")
            cls.spec.verify_files(cls.input, False)
            cls.workspace.stage.parent.mkdir()
            os.rename(cls.input, cls.workspace.stage)
            try:
                if not (cls.workspace.stage / ".git").exists():
                    install_exact_git(cls.workspace.stage, cls.spec)
            except BaseException:
                os.rename(cls.workspace.stage, cls.input)
                raise
        else:
            cls.workspace.download()
            M.git(cls.workspace.source, "apply", "--reverse", "--unidiff-zero", "-p1", "--", str(cls.spec.patch))
            cls.workspace.receipt.unlink()
            os.rename(cls.workspace.source, cls.workspace.stage)
        M.verify_git(cls.workspace.stage, cls.spec)
        cls.spec.verify_files(cls.workspace.stage, False)

    @classmethod
    def tearDownClass(cls):
        if cls.offline:
            current = cls.workspace.stage if cls.workspace.stage.exists() else cls.workspace.source
            os.rename(current, cls.input)
            cls.spec.verify_files(cls.input, False)
        shutil.rmtree(cls.directory)

    def test_full_pin_publication_and_readonly_repeat(self):
        w = self.workspace
        owner = w.owner()
        before = {n: M.digest(M.read_file(w.stage / n, e["size"])) for n, e in self.spec.files.items()}
        M.write_exclusive(w.admission, b"test admission\n")
        self.assertFalse(w.receipt.exists())
        w.publish(owner)
        try:
            self.assertFalse(w.stage.exists())
            self.assertTrue(w.receipt.exists())
            w.verify()
            paths = [w.owner_path, w.receipt, *[w.source / p for p in M.inventory(w.source)]]
            seal = {str(p): (M.digest(p.read_bytes()), os.lstat(p).st_mtime_ns) for p in paths}
            with mock.patch.object(M, "write_exclusive", side_effect=AssertionError("repeat wrote")):
                w.verify()
                w.preflight(True)
                w.download()
            self.assertEqual(seal, {str(p): (M.digest(p.read_bytes()), os.lstat(p).st_mtime_ns) for p in paths})
            changed = [n for n, e in self.spec.files.items() if e["pristine_sha256"] != e["patched_sha256"]]
            self.assertEqual(len(changed), 33)
            self.assertEqual(len(before), 1118)
            for n, e in self.spec.files.items():
                self.assertEqual(M.digest((w.source / n).read_bytes()), e["patched_sha256"])
                if n not in changed:
                    self.assertEqual(before[n], e["patched_sha256"])
            for field in ("owner_sha256", "source", "delivery", "index_sha256"):
                raw = w.receipt.read_bytes()
                value = json.loads(raw)
                value[field] = "mismatch"
                w.receipt.write_bytes(M.encoded(value))
                with self.assertRaises((ValueError, TypeError)):
                    w.verify()
                self.assertEqual(w.receipt.read_bytes(), M.encoded(value))
                w.receipt.write_bytes(raw)
        finally:
            M.git(w.source, "apply", "--reverse", "--unidiff-zero", "-p1", "--", str(self.spec.patch))
            w.receipt.unlink()
            os.rename(w.source, w.stage)

    def test_every_changed_file_preserves_operations_defaults_and_enum_declarations(self):
        source = self.workspace.stage
        changed = [n for n, e in self.spec.files.items() if e["pristine_sha256"] != e["patched_sha256"]]
        pristine = {n: (source / n).read_bytes() for n in changed}
        M.git(source, "apply", "--unidiff-zero", "-p1", "--", str(self.spec.patch))
        try:
            block = "".join("\tusing " + n + " = LengthPercentageAuto;\n" for n in ("Top", "Right", "Bottom", "Left"))
            for name, before in pristine.items():
                after = (source / name).read_text()
                restored = re.sub(r"\binitial_(\w+)\b", r"\1", after)
                original = before.decode()
                if name.endswith("/StyleTypes.h"):
                    self.assertEqual(after.count(block), 1)
                    self.assertGreater(after.index(block), after.index("enum class OriginY"))
                    self.assertEqual(re.findall(r"enum class[^;]+;", original), re.findall(r"enum class[^;]+;", after))
                    restored = restored.replace(block, "")
                    original = original.replace(block, "")
                self.assertEqual(restored, original, name)
        finally:
            M.git(source, "apply", "--reverse", "--unidiff-zero", "-p1", "--", str(self.spec.patch))

    def test_interruption_at_each_publication_boundary_never_marks_incomplete_ready(self):
        w = self.workspace
        real_git, real_rename, real_write = M.git, M.os.rename, M.write_exclusive
        real_unlink = Path.unlink
        for boundary in ("check", "apply", "postverify", "rename", "receipt", "admission"):
            with self.subTest(boundary=boundary):
                M.write_exclusive(w.admission, b"controlled admission\n")
                owner = w.owner()
                def command(source, *args, **kwargs):
                    if args[0] == "apply" and ((boundary == "check" and "--check" in args)
                                               or (boundary == "apply" and "--check" not in args)):
                        raise KeyboardInterrupt(boundary)
                    return real_git(source, *args, **kwargs)
                calls = 0
                real_verify = self.spec.verify_files
                def verify(source, patched):
                    nonlocal calls
                    calls += 1
                    if boundary == "postverify" and patched:
                        raise KeyboardInterrupt(boundary)
                    real_verify(source, patched)
                def rename(source, destination):
                    self.assertFalse(w.receipt.exists())
                    if boundary == "rename":
                        raise KeyboardInterrupt(boundary)
                    real_rename(source, destination)
                def write(path, data):
                    self.assertFalse(w.stage.exists())
                    self.assertTrue(w.source.exists())
                    if boundary == "receipt":
                        real_write(path, b"{partial")
                        raise KeyboardInterrupt(boundary)
                    real_write(path, data)
                def unlink(path, *args, **kwargs):
                    if boundary == "admission" and path == w.admission:
                        raise KeyboardInterrupt(boundary)
                    return real_unlink(path, *args, **kwargs)
                try:
                    with mock.patch.object(M, "git", side_effect=command), mock.patch.object(M.os, "rename", side_effect=rename), mock.patch.object(M, "write_exclusive", side_effect=write), mock.patch.object(self.spec, "verify_files", side_effect=verify), mock.patch.object(Path, "unlink", autospec=True, side_effect=unlink):
                        with self.assertRaises(KeyboardInterrupt):
                            w.publish(owner)
                    with self.assertRaises((ValueError, OSError)):
                        w.preflight(False)
                    self.assertTrue(w.admission.exists())
                finally:
                    if w.source.exists():
                        real_rename(w.source, w.stage)
                    try:
                        self.spec.verify_files(w.stage, False)
                    except M.Refusal:
                        real_git(w.stage, "apply", "--reverse", "--unidiff-zero", "-p1", "--", str(self.spec.patch))
                    if w.receipt.exists():
                        w.receipt.unlink()
                    w.admission.unlink()
                self.spec.verify_files(w.stage, False)

    def test_files_mixed_missing_extra_and_partial_refuse_untouched(self):
        source = self.workspace.stage
        self.spec.verify_files(source, False)
        for name in ("Include/RmlUi/Core/StyleTypes.h", "Source/Core/Geometry.cpp", "LICENSE.txt"):
            if name not in self.spec.files:
                name = next(iter(self.spec.files))
            path = source / name
            original = path.read_bytes()
            for replacement in (b"", original[:-1], original + b"\n"):
                path.write_bytes(replacement)
                with self.assertRaises(M.Refusal):
                    self.spec.verify_files(source, False)
                self.assertEqual(path.read_bytes(), replacement)
            path.unlink()
            with self.assertRaises(M.Refusal):
                self.spec.verify_files(source, False)
            self.assertFalse(path.exists())
            path.write_bytes(original)
        for directory in (False, True):
            extra = source / "untracked-input"
            extra.mkdir() if directory else extra.write_bytes(b"foreign")
            with self.assertRaises(M.Refusal):
                self.spec.verify_files(source, False)
            self.assertTrue(extra.exists())
            extra.rmdir() if directory else extra.unlink()
        M.git(source, "apply", "--unidiff-zero", "-p1", "--", str(self.spec.patch))
        try:
            name = "Include/RmlUi/Core/StyleTypes.h"
            patched = (source / name).read_bytes()
            (source / name).write_bytes(b"mixed")
            with self.assertRaises(M.Refusal):
                self.spec.verify_files(source, True)
            (source / name).write_bytes(patched)
        finally:
            M.git(source, "apply", "--reverse", "--unidiff-zero", "-p1", "--", str(self.spec.patch))

    def test_git_pin_index_modes_flags_alternates_and_config_refusals(self):
        source = self.workspace.stage
        admin = source / ".git"
        for name, replacement in (("HEAD", b"0" * 40 + b"\n"), ("index", b"partial-index"),
                                  ("config", b"[include]\npath = outside\n")):
            path = admin / name
            original = path.read_bytes()
            try:
                path.write_bytes(replacement)
                with self.assertRaises((M.Refusal, ValueError)):
                    M.verify_git(source, self.spec)
                self.assertEqual(path.read_bytes(), replacement)
            finally:
                path.write_bytes(original)
        for name in ("commondir", "objects/info/alternates", "objects/info/http-alternates", "info/grafts"):
            path = admin / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"foreign\n")
            with self.assertRaises(M.Refusal):
                M.verify_git(source, self.spec)
            self.assertEqual(path.read_bytes(), b"foreign\n")
            path.unlink()
        original = (admin / "index").read_bytes()
        name = next(iter(self.spec.files))
        for args in (("--assume-unchanged",), ("--skip-worktree",), ("--chmod=+x",)):
            try:
                M.git(source, "update-index", *args, "--", name)
                with self.assertRaises(M.Refusal):
                    M.verify_git(source, self.spec)
            finally:
                (admin / "index").write_bytes(original)
        M.verify_git(source, self.spec)

    def test_nonordinary_files_and_links(self):
        source = self.workspace.stage
        name = next(iter(self.spec.files))
        path = source / name
        original = path.read_bytes()
        path.unlink()
        try:
            with mock.patch.object(M.os, "lstat", return_value=mock.Mock(st_mode=stat.S_IFLNK, st_file_attributes=0)):
                with self.assertRaises(M.Refusal):
                    M.read_file(path, len(original))
            if os.name == "posix":
                os.mkfifo(path)
                with self.assertRaises(M.Refusal):
                    self.spec.verify_files(source, False)
                path.unlink()
            target = source / "link-target"
            target.write_bytes(original)
            os.link(target, path)
            with self.assertRaises(M.Refusal):
                M.read_file(path, len(original))
            path.unlink()
            target.unlink()
            with mock.patch.object(M.os, "symlink", side_effect=PermissionError("creation denied")):
                with self.assertRaises(PermissionError):
                    M.os.symlink("outside", path)
                self.assertFalse(path.exists())
        finally:
            if os.path.lexists(path):
                path.unlink()
            path.write_bytes(original)

    def test_mode_and_unreadable_refuse(self):
        path = self.workspace.stage / next(iter(self.spec.files))
        if os.name == "posix":
            original = path.stat().st_mode
            try:
                path.chmod(original | stat.S_IXUSR)
                with self.assertRaises(M.Refusal):
                    self.spec.verify_files(self.workspace.stage, False)
            finally:
                path.chmod(original)
        with mock.patch.object(M.os, "open", side_effect=PermissionError("unreadable")):
            with self.assertRaises(PermissionError):
                M.read_file(path, 1024)


class DeliveryRefusals(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="rmlui-refusal-")
        self.addCleanup(self.directory.cleanup)
        self.spec = specification()
        self.w = M.Workspace(Path(self.directory.name).resolve(strict=True) / "build", self.spec)

    def test_init_never_adopts_existing_file_directory_or_owner(self):
        self.w.root.write_bytes(b"foreign")
        with self.assertRaises(M.Refusal):
            self.w.initialize()
        self.assertEqual(self.w.root.read_bytes(), b"foreign")
        self.w.root.unlink()
        self.w.root.mkdir()
        with self.assertRaises(M.Refusal):
            self.w.initialize()
        self.assertEqual(list(self.w.root.iterdir()), [])
        self.w.root.rmdir()
        self.w.initialize()
        owner = self.w.owner_path.read_bytes()
        with self.assertRaises(M.Refusal):
            self.w.initialize()
        self.assertEqual(self.w.owner_path.read_bytes(), owner)

    def test_all_delivery_fields_and_missing_partial_ownership_refuse(self):
        self.w.initialize()
        original = self.w.owner_path.read_bytes()
        for field in self.spec.delivery:
            value = json.loads(original)
            value["delivery"][field] = "mismatch"
            data = M.encoded(value)
            self.w.owner_path.write_bytes(data)
            with self.assertRaises(M.Refusal):
                self.w.preflight(False)
            self.assertEqual(self.w.owner_path.read_bytes(), data)
        for data in (b"", b"{", b"{}", b"null"):
            self.w.owner_path.write_bytes(data)
            with self.assertRaises((ValueError, TypeError)):
                self.w.preflight(False)
            self.assertEqual(self.w.owner_path.read_bytes(), data)
        self.w.owner_path.unlink()
        with self.assertRaises(OSError):
            self.w.preflight(False)
        self.assertFalse(self.w.owner_path.exists())

    def test_missing_source_fully_disconnected_and_every_occupant_refuses(self):
        self.w.initialize()
        with self.assertRaises(M.Refusal):
            self.w.preflight(True)
        for path in (self.w.source, self.w.stage, self.w.admission, self.w.receipt):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"foreign")
            with self.assertRaises((ValueError, OSError)):
                self.w.preflight(False)
            self.assertEqual(path.read_bytes(), b"foreign")
            path.unlink()
        self.w.preflight(False)

    def test_interrupted_download_has_no_ready_and_cannot_resume(self):
        self.w.initialize()
        with mock.patch.object(M, "git", side_effect=KeyboardInterrupt):
            with self.assertRaises(KeyboardInterrupt):
                self.w.download()
        self.assertTrue(self.w.stage.exists())
        self.assertTrue(self.w.admission.exists())
        self.assertFalse(self.w.receipt.exists())
        with self.assertRaises(M.Refusal):
            self.w.download()

    def test_two_exclusive_admissions_never_share_stage(self):
        self.w.initialize()
        entered = threading.Event()
        release = threading.Event()
        failures = []

        def fake_git(*args):
            entered.set()
            if not release.wait(10):
                raise AssertionError("fixture barrier did not release")
            raise M.Refusal("controlled fetch failure")

        def invoke():
            try:
                self.w.download()
            except (M.Refusal, OSError) as error:
                failures.append(str(error))

        with mock.patch.object(M, "git", side_effect=fake_git):
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
                first = executor.submit(invoke)
                try:
                    self.assertTrue(entered.wait(10))
                    second = executor.submit(invoke)
                    second.result(timeout=10)
                    self.assertEqual(len(failures), 1)
                finally:
                    release.set()
                first.result(timeout=10)
        self.assertEqual(len(failures), 2)
        self.assertFalse(self.w.receipt.exists())
        self.assertEqual(self.w.admission.read_bytes(), b"exclusive source delivery\n")

    def test_path_escape_drive_relative_alias_and_reparse_refuse(self):
        for name in ("../file", "/file", "a//b", "a/./b", "a/../b", "a\\b", "C:thing", "NUL", "aux.txt", "file.", "file ", "short~1"):
            with self.subTest(name=name), self.assertRaises(M.Refusal):
                M.safe_relative(name)
        for name in ("", "C:relative", "a/../b"):
            with self.subTest(name=name), self.assertRaises(M.Refusal):
                M.canonical(name)
        info = mock.Mock(st_mode=stat.S_IFDIR, st_file_attributes=M.REPARSE)
        with mock.patch.object(M.os, "lstat", return_value=info):
            with self.assertRaises(M.Refusal):
                M.canonical(self.w.root.parent)

    def test_cmake_and_ci_registration_preserve_population_and_verification_order(self):
        cmake = (ROOT / "cmake/rmlui_source.cmake").read_text()
        self.assertLess(cmake.index("preflight --root"), cmake.index("FetchContent_Declare(RmlUi"))
        self.assertLess(cmake.index("FetchContent_MakeAvailable(RmlUi)"), cmake.index("verify --root"))
        self.assertLess(cmake.index("verify --root"), cmake.index("add_subdirectory("))
        self.assertIn('NOT "${FETCHCONTENT_SOURCE_DIR_RMLUI}" STREQUAL ""', cmake)
        self.assertIn('PROPERTY __FETCHCONTENT_MAKEAVAILABLE_SERIAL_PROVIDER SET', cmake)
        self.assertIn('UPDATE_COMMAND ""', cmake)
        self.assertIn('PATCH_COMMAND ""', cmake)
        self.assertIn('SOURCE_DIR "${_rmlui_population}"', cmake)
        self.assertIn('set(rmlui_SOURCE_DIR "${_rmlui_expected_source}")', cmake)
        for refusal in ("FETCHCONTENT_SOURCE_DIR_RMLUI", "_rmlui_provider", "rmlui_POPULATED", "FETCHCONTENT_BASE_DIR"):
            self.assertIn(refusal, cmake)
        self.assertIn('"tools/ci/test_rmlui_source.py"', (ROOT / "tools/ci/ci_paths.py").read_text())
        self.assertIn("RmlUiSourceScriptContract", (ROOT / "tools/CMakeLists.txt").read_text())
        for workflow in ("ci.yml", "asan.yml", "performance.yml", "release.yml"):
            text = (ROOT / ".github/workflows" / workflow).read_text()
            self.assertIn("rmlui_source.py init --root", text)
        self.assertIn("tools/ci/ci_paths.py python-tests", (ROOT / ".github/workflows/ci.yml").read_text())

    def test_git_live_rename_is_transient_but_final_scan_and_other_errors_are_strict(self):
        real_process, real_inventory = subprocess.Popen, M.inventory
        for error, final_error in ((FileNotFoundError, False), (PermissionError, False),
                                   (FileNotFoundError, True)):
            with self.subTest(error=error.__name__, final_error=final_error):
                observed = threading.Event()
                calls = 0
                def scan(source):
                    nonlocal calls
                    calls += 1
                    if calls == 1:
                        observed.set()
                        raise error("controlled administration entry")
                    if final_error:
                        raise FileNotFoundError("final scan remains strict")
                    return real_inventory(source)
                class Output:
                    def __init__(self, stream):
                        self.stream = stream
                    def __enter__(self):
                        self.stream.__enter__()
                        return self
                    def __exit__(self, *args):
                        return self.stream.__exit__(*args)
                    def read(self, count):
                        if not observed.wait(5):
                            raise AssertionError("storage monitor did not run")
                        return self.stream.read(count)
                def launch(*args, **kwargs):
                    process = real_process(*args, **kwargs)
                    process.stdout = Output(process.stdout)
                    return process
                with mock.patch.object(M, "inventory", side_effect=scan), mock.patch.object(M.subprocess, "Popen", side_effect=launch):
                    if error is FileNotFoundError and not final_error:
                        self.assertTrue(M.git(self.w.root.parent, "--version").startswith(b"git version"))
                        self.assertGreaterEqual(calls, 2)
                    else:
                        with self.assertRaises((M.Refusal, FileNotFoundError)):
                            M.git(self.w.root.parent, "--version")
                self.assertFalse(self.w.root.exists())

    def test_population_is_empty_ordinary_and_never_evaluates_foreign_cmake(self):
        self.w.initialize()
        self.w.preflight(False)
        self.w.population.parent.mkdir()
        self.w.population.mkdir()
        self.w.preflight(False)
        for name in ("CMakeLists.txt", "rmlui-population-only-" + M.SOURCE_ID + "/CMakeLists.txt"):
            with self.subTest(path=name):
                file = self.w.population / name
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_bytes(b"foreign source must never be evaluated")
                with self.assertRaises(M.Refusal):
                    self.w.preflight(False)
                self.assertEqual(file.read_bytes(), b"foreign source must never be evaluated")
                file.unlink()
                if file.parent != self.w.population:
                    file.parent.rmdir()
        self.w.population.rmdir()
        self.w.population.write_bytes(b"foreign population file")
        with self.assertRaises(M.Refusal):
            self.w.preflight(False)
        self.assertEqual(self.w.population.read_bytes(), b"foreign population file")
        self.w.population.unlink()
        self.w.population.mkdir()
        with mock.patch.object(M.os, "scandir", side_effect=PermissionError("unreadable population")):
            with self.assertRaises(PermissionError):
                self.w.preflight(False)
        self.assertEqual(list(self.w.population.iterdir()), [])
        self.assertFalse(self.w.receipt.exists())

    def test_delivery_inputs_select_python_and_native_lanes(self):
        loader = importlib.util.spec_from_file_location("ci_paths", ROOT / "tools/ci/ci_paths.py")
        paths = importlib.util.module_from_spec(loader)
        loader.loader.exec_module(paths)
        for name in ("cmake/rmlui_source.cmake", "cmake/patches/rmlui-6.1-shadow.patch",
                     "vendor/rmlui/rmlui-6.1-source.json", "tools/ci/rmlui_source.py",
                     "tools/ci/test_rmlui_source.py"):
            with self.subTest(path=name):
                self.assertTrue(paths.needs_python(name))
                self.assertTrue(paths.needs_cpp(name))

    def test_manifest_metadata_path_matches_helper_cmake_and_lf_checkout(self):
        manifest = "vendor/rmlui/rmlui-6.1-source.json"
        result = subprocess.run([sys.executable, "-I", "-B", str(MODULE_PATH),
                                 "init", "--root", str(self.w.root)],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(self.w.owner_path.read_bytes())["delivery"], self.spec.delivery)
        self.assertEqual(self.spec.manifest_bytes, (ROOT / manifest).read_bytes())
        self.assertFalse((ROOT / "cmake/patches/rmlui-6.1-source.json").exists())
        self.assertIn('repository / "' + manifest + '"', MODULE_PATH.read_text())
        self.assertIn('set(_rmlui_manifest "${CMAKE_SOURCE_DIR}/' + manifest + '")',
                      (ROOT / "cmake/rmlui_source.cmake").read_text())
        result = subprocess.run(["git", "check-attr", "text", "eol", "--", manifest],
                                cwd=ROOT, capture_output=True, text=True, check=True,
                                env=dict(os.environ, GIT_OPTIONAL_LOCKS="0"))
        self.assertEqual(result.stdout.splitlines(),
                         [manifest + ": text: set", manifest + ": eol: lf"])
        self.assertNotIn(b"\r", self.spec.manifest_bytes)

    def test_ucrt_enrollment_failure_stops_configure_and_legacy_tree_remains_usable(self):
        sys.path.insert(0, str(MODULE_PATH.parent))
        from ci_shell import bash_executable
        script = (ROOT / "tools/ci/ucrt_attribution.sh").read_text()
        begin = script.index("    # Legacy source trees")
        end = script.index("    if (( rc != 0 )); then", begin)
        fragment = script[begin:end]
        with tempfile.TemporaryDirectory(prefix="rmlui-caller-") as directory:
            root = Path(directory).resolve(strict=True)
            (root / "cmake").mkdir()
            (root / "logs").mkdir()
            module = root / "cmake/rmlui_source.cmake"
            for current, initialization_exit in ((True, 0), (True, 7), (False, 0)):
                if current:
                    module.write_text("delivery module")
                elif module.exists():
                    module.unlink()
                harness = ("rc=0; dir=logs; bdir=build/cell-0\n"
                           "bounded() { echo \"$1\" >> calls; "
                           f"if [[ $1 == python ]]; then return {initialization_exit}; fi; }}\n"
                           + fragment + "\nprintf '%s' \"$rc\"\n")
                result = subprocess.run([bash_executable(), "-c", harness], cwd=root,
                                        capture_output=True, text=True, check=True)
                calls = (root / "calls").read_text().splitlines()
                self.assertEqual(calls, (["python", "cmake"] if initialization_exit == 0 else ["python"])
                                 if current else ["cmake"])
                self.assertEqual(result.stdout, str(initialization_exit if current else 0))
                (root / "calls").unlink()

    def test_atomic_exclusive_creation_collision_and_flush_failure(self):
        path = Path(self.directory.name).resolve(strict=True) / "record"
        path.write_bytes(b"foreign")
        with self.assertRaises(FileExistsError):
            M.write_exclusive(path, b"ours")
        self.assertEqual(path.read_bytes(), b"foreign")
        path.unlink()
        with mock.patch.object(M.os, "fsync", side_effect=OSError("flush failure")):
            with self.assertRaises(OSError):
                M.write_exclusive(path, b"partial operation")
        self.assertEqual(path.read_bytes(), b"partial operation")

    def test_windows_long_names_drive_paths_and_api_failure(self):
        M.check_windows_spelling("C:/Package Files/build", "c:\\Package Files\\build")
        for long_name in ("", "C:/Package Files/build"):
            with self.assertRaises(M.Refusal):
                M.check_windows_spelling("C:/PACKAG~1/build", long_name)
        kernel = mock.Mock()
        kernel.GetLongPathNameW.return_value = 0
        with mock.patch("ctypes.windll", mock.Mock(kernel32=kernel), create=True):
            with self.assertRaises(M.Refusal):
                M.windows_long_name("C:/unreadable")

    def test_empty_relative_path_and_missing_executable_refuse(self):
        source = Path(self.directory.name).resolve(strict=True)
        for path in ("", ".", "relative", os.pathsep + str(source)):
            with mock.patch.dict(M.os.environ, {"PATH": path}):
                with self.assertRaises(M.Refusal):
                    M.git(source, "init")
        with mock.patch.object(M.shutil, "which", return_value=None):
            with self.assertRaises(M.Refusal):
                M.git(source, "init")

    def test_wrong_patch_manifest_pin_and_helper_identity(self):
        patch = Path(self.directory.name).resolve(strict=True) / "patch"
        manifest = Path(self.directory.name).resolve(strict=True) / "manifest.json"
        patch.write_bytes(self.spec.patch_bytes[:-1])
        manifest.write_bytes(self.spec.manifest_bytes)
        with self.assertRaises(M.Refusal):
            M.Specification(manifest, patch)
        patch.write_bytes(self.spec.patch_bytes)
        for field in ("revision", "source_identity", "schema", "file_count"):
            data = json.loads(self.spec.manifest_bytes)
            data[field] = "wrong"
            manifest.write_bytes(M.encoded(data))
            with self.assertRaises(M.Refusal):
                M.Specification(manifest, patch)
        self.w.initialize()
        with mock.patch.object(M, "__file__", str(patch)):
            changed = specification()
            self.assertNotEqual(self.spec.delivery["helper_sha256"], changed.delivery["helper_sha256"])
            with self.assertRaises(M.Refusal):
                M.Workspace(self.w.root, changed).preflight(False)


class PairedWorkflowInitialization(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="rmlui-paired-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve(strict=True)
        for checkout in ("base", "head"):
            for name in ("tools/ci/rmlui_source.py", "vendor/rmlui/rmlui-6.1-source.json",
                         "cmake/patches/rmlui-6.1-shadow.patch"):
                destination = self.root / checkout / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes((ROOT / name).read_bytes())
        self.base_helper = self.root / "base/tools/ci/rmlui_source.py"
        # Both helpers remain valid; ownership must distinguish their bytes.
        with self.base_helper.open("ab") as stream:
            stream.write(b"\n# Distinct source revision.\n")
        workflow = (ROOT / ".github/workflows/performance.yml").read_text()
        marker = "      - name: Configure identical builds\n        run: |\n"
        self.assertEqual(workflow.count(marker), 1)
        self.fragment = textwrap.dedent(workflow.split(marker, 1)[1].split("\n      - name:", 1)[0])
        # Stop at configure boundaries: run only the source checkout's real
        # ownership preflight, never project configure, download or compilation.
        (self.root / "boundary.py").write_text(textwrap.dedent("""\
            import json
            from pathlib import Path
            import subprocess
            import sys

            args = sys.argv[1:]
            source = Path(args[args.index("-S") + 1])
            build = Path(args[args.index("-B") + 1])
            with Path("configure-calls.jsonl").open("a") as stream:
                stream.write(json.dumps(args) + "\\n")
            helper = source / "tools/ci/rmlui_source.py"
            if helper.is_file():
                sys.exit(subprocess.run([sys.executable, "-I", "-B", str(helper),
                                         "preflight", "--root", str(build)], check=False).returncode)
            if build.exists():
                raise AssertionError("legacy base was enrolled")
            """), encoding="utf-8")

    def run_workflow(self):
        sys.path.insert(0, str(MODULE_PATH.parent))
        from ci_shell import bash_executable
        harness = ('python3() { "$FIXTURE_PYTHON" "$@"; }\n'
                   'cmake() { python3 -I -B boundary.py "$@"; }\n' + self.fragment)
        return subprocess.run([bash_executable(), "-e", "-o", "pipefail", "-c", harness],
                              cwd=self.root, capture_output=True, text=True, check=False,
                              env=dict(os.environ, FIXTURE_PYTHON=Path(sys.executable).as_posix(),
                                       GITHUB_WORKSPACE=self.root.as_posix()))

    def calls(self):
        path = self.root / "configure-calls.jsonl"
        return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []

    def assert_owner(self, checkout):
        root = self.root / "build" / checkout
        owner = json.loads((root / ".rmlui-owner.json").read_bytes())
        expected = dict(specification().delivery)
        expected["helper_sha256"] = M.digest((self.root / checkout / "tools/ci/rmlui_source.py").read_bytes())
        self.assertEqual(owner["delivery"], expected)
        self.assertEqual(owner["root"], str(M.canonical(root)))
        self.assertEqual(set(p.name for p in root.iterdir()), {".rmlui-owner.json"})
        return owner["delivery"]

    def test_distinct_revisions_initialize_with_their_own_helpers(self):
        result = self.run_workflow()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        base = self.assert_owner("base")
        head = self.assert_owner("head")
        self.assertNotEqual(base["helper_sha256"], head["helper_sha256"])
        self.assertEqual([args[:4] for args in self.calls()],
                         [["-S", "base", "-B", "build/base"], ["-S", "head", "-B", "build/head"]])

    def test_legacy_base_is_not_enrolled_and_head_keeps_its_own_helper(self):
        self.base_helper.unlink()
        result = self.run_workflow()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((self.root / "build/base").exists())
        self.assert_owner("head")
        self.assertEqual([args[:4] for args in self.calls()],
                         [["-S", "base", "-B", "build/base"], ["-S", "head", "-B", "build/head"]])

    def test_base_initialization_refusals_stop_before_configure(self):
        manifest = self.root / "base/vendor/rmlui/rmlui-6.1-source.json"
        original = manifest.read_bytes()
        for data in (None, b"{", b"{}"):
            with self.subTest(manifest=data):
                if data is None:
                    manifest.unlink()
                else:
                    manifest.write_bytes(data)
                try:
                    result = self.run_workflow()
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(self.calls(), [])
                    self.assertFalse((self.root / "build").exists())
                finally:
                    manifest.write_bytes(original)
        build = self.root / "build/base"
        build.mkdir(parents=True)
        (build / "foreign").write_bytes(b"preserve")
        result = self.run_workflow()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.calls(), [])
        self.assertEqual(list(build.iterdir()), [build / "foreign"])
        self.assertEqual((build / "foreign").read_bytes(), b"preserve")
        self.assertFalse((self.root / "build/head").exists())


class HostedConfigurationContracts(unittest.TestCase):
    def test_dependency_configuration_keeps_the_original_shared_option_order(self):
        vendor = (ROOT / "vendor/CMakeLists.txt").read_text()
        module = (ROOT / "cmake/rmlui_source.cmake").read_text()
        calls = re.findall(r'^FetchContent_MakeAvailable\(([^)]*)\)|'
                           r'^include\("\$\{CMAKE_CURRENT_LIST_DIR\}/../cmake/rmlui_source.cmake"\)',
                           vendor, re.MULTILINE)
        # The owned hook verifies the full source before configuring RmlUi.
        self.assertEqual(module.count('add_subdirectory("${rmlui_SOURCE_DIR}" "${rmlui_BINARY_DIR}")'), 1)
        self.assertLess(module.index("verify --root"), module.index("add_subdirectory("))
        order = []
        for arguments in calls:
            order.extend(arguments.split() if arguments else ["RmlUi"])
        self.assertEqual(order, ["freetype", "EnTT", "glm", "glfw", "glad", "nlohmann_json",
                                 "JoltPhysics", "RmlUi", "soil2", "miniaudio", "meshoptimizer",
                                 "lua_upstream", "sol2", "spdlog", "imgui_src", "stb",
                                 "googletest", "lz4_upstream"])
        self.assertNotIn("BUILD_SHARED_LIBS", vendor + module)

    def test_fresh_fixture_paths_are_resolved_before_strict_workspace_validation(self):
        with tempfile.TemporaryDirectory(prefix="rmlui-fixture-path-") as directory:
            root = Path(directory).resolve(strict=True)
            (root / "alias").mkdir()
            alias = str(root / "alias" / "..")
            with self.assertRaises(M.Refusal):
                M.canonical(alias)
            # This spelling models an aliased TEMP without symlink privileges.
            # Each fixture still allocates its own fresh directory underneath it.
            allocate = tempfile.mkdtemp
            def allocate_alias(*args, **kwargs):
                allocated = Path(allocate(*args, **kwargs))
                return str(allocated.parent / "alias" / ".." / allocated.name)
            with mock.patch.object(tempfile, "tempdir", str(root)), \
                    mock.patch.object(tempfile, "mkdtemp", side_effect=allocate_alias):
                refusal = DeliveryRefusals()
                try:
                    refusal.setUp()
                    self.assertEqual(refusal.w.root, refusal.w.root.resolve())
                    refusal.w.initialize()
                    refusal.w.preflight(False)
                finally:
                    refusal.doCleanups()

    def test_paired_fixture_resolves_its_fresh_allocation_before_workflow(self):
        with tempfile.TemporaryDirectory(prefix="rmlui-paired-path-") as directory:
            root = Path(directory).resolve(strict=True)
            (root / "alias").mkdir()
            allocate = tempfile.mkdtemp
            def allocate_alias(*args, **kwargs):
                allocated = Path(allocate(*args, **kwargs))
                return str(allocated.parent / "alias" / ".." / allocated.name)
            with mock.patch.object(tempfile, "tempdir", str(root)), \
                    mock.patch.object(tempfile, "mkdtemp", side_effect=allocate_alias):
                paired = PairedWorkflowInitialization()
                try:
                    paired.setUp()
                    self.assertEqual(paired.root, M.canonical(paired.root))
                    result = paired.run_workflow()
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    paired.assert_owner("base")
                    paired.assert_owner("head")
                finally:
                    paired.doCleanups()

    def test_full_source_fixture_resolves_its_fresh_allocation_before_workspace(self):
        class BoundaryReached(Exception):
            pass

        class Fixture(SourceFiles):
            pass

        with tempfile.TemporaryDirectory(prefix="rmlui-source-path-") as directory:
            root = Path(directory).resolve(strict=True)
            (root / "alias").mkdir()
            allocate = tempfile.mkdtemp
            def allocate_alias(*args, **kwargs):
                allocated = Path(allocate(*args, **kwargs))
                return str(allocated.parent / "alias" / ".." / allocated.name)
            with mock.patch.object(tempfile, "tempdir", str(root)), \
                    mock.patch.object(tempfile, "mkdtemp", side_effect=allocate_alias), \
                    mock.patch.object(M.Workspace, "initialize", side_effect=BoundaryReached):
                try:
                    with self.assertRaises(BoundaryReached):
                        Fixture.setUpClass()
                    self.assertEqual(Fixture.directory, M.canonical(Fixture.directory))
                finally:
                    if "directory" in Fixture.__dict__:
                        shutil.rmtree(Fixture.directory)


if __name__ == "__main__":
    unittest.main()
