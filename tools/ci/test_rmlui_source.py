#!/usr/bin/env python3
"""Exercise delivery using the complete immutable vendor input and real Git apply.

An offline author may supply a pristine archive extraction directly under TMPDIR
with LUMINUMBRA_RMLUI_TEST_SOURCE. It is temporarily moved, never copied or used
as a production source override. The caller must give each direct CLI process
exclusive ownership of this input until it returns; the CTest resource lock only
coordinates the registered CTest consumers. Otherwise the fixture downloads
the exact pin.
"""
from __future__ import annotations

import concurrent.futures
import contextlib
import io
import hashlib
import importlib.util
import itertools
import json
import os
import re
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import time
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


def file_identity(path):
    info = os.lstat(path)
    return info.st_dev, info.st_ino


def clear_fixture_readonly(path, expected):
    """Clear only the readonly attribute through an exclusively opened own file."""
    import ctypes
    from ctypes import wintypes
    import msvcrt
    api = ctypes.WinDLL("kernel32", use_last_error=True)
    api.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                               ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    api.CreateFileW.restype = wintypes.HANDLE
    api.CloseHandle.argtypes = [wintypes.HANDLE]
    api.CloseHandle.restype = wintypes.BOOL
    class BasicInfo(ctypes.Structure):
        _fields_ = [(name, ctypes.c_longlong) for name in
                    ("CreationTime", "LastAccessTime", "LastWriteTime", "ChangeTime")]
        _fields_ += [("FileAttributes", wintypes.DWORD)]
    for name in ("GetFileInformationByHandleEx", "SetFileInformationByHandle"):
        method = getattr(api, name)
        method.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
        method.restype = wintypes.BOOL
    # Share mode zero refuses live users/writers before any attribute mutation.
    handle = api.CreateFileW(str(path), 0x80 | 0x100, 0, None, 3, 0x00200000, None)
    if handle == wintypes.HANDLE(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        fd = msvcrt.open_osfhandle(handle, os.O_RDONLY)
    except BaseException:
        api.CloseHandle(handle)
        raise
    try:
        info = os.fstat(fd)
        if (info.st_dev, info.st_ino) != expected or not M.ordinary(info) or info.st_nlink != 1:
            raise AssertionError("fixture file custody changed")
        basic = BasicInfo()
        if not api.GetFileInformationByHandleEx(handle, 0, ctypes.byref(basic), ctypes.sizeof(basic)):
            raise ctypes.WinError(ctypes.get_last_error())
        if not basic.FileAttributes & stat.FILE_ATTRIBUTE_READONLY:
            raise AssertionError("cleanup failure was not a readonly file")
        basic.FileAttributes &= ~stat.FILE_ATTRIBUTE_READONLY
        if not api.SetFileInformationByHandle(handle, 0, ctypes.byref(basic), ctypes.sizeof(basic)):
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        os.close(fd)


def remove_source_fixture(directory, expected_directory, sources):
    """Remove a verified fresh test allocation; foreign or busy files fail closed."""
    if file_identity(directory) != expected_directory or directory != M.canonical(directory):
        raise AssertionError("fixture directory custody changed")
    identities = {directory / name: file_identity(directory / name) for name in M.inventory(directory)}
    def retry(function, filename, error_info):
        error = error_info[1]
        path = Path(filename)
        if (function is not os.unlink or not isinstance(error, PermissionError)
                or getattr(error, "winerror", None) != 5
                or file_identity(directory) != expected_directory or path not in identities):
            raise error
        # Git marks pack/loose object files readonly. Only those exact owned files
        # qualify; reparse points, links, ordinary access errors and locks do not.
        eligible = any(path.is_relative_to(source / ".git") and
                       re.fullmatch(r"objects/(?:pack/pack-[0-9a-f]{40}\.(?:pack|idx|rev)|[0-9a-f]{2}/[0-9a-f]{38})",
                                    path.relative_to(source / ".git").as_posix())
                       for source in sources)
        info = os.lstat(path)
        if (not eligible or not M.ordinary(info) or info.st_nlink != 1
                or not getattr(info, "st_file_attributes", 0) & 1
                or file_identity(path) != identities[path]):
            raise error
        clear_fixture_readonly(path, identities[path])
        if file_identity(directory) != expected_directory or file_identity(path) != identities[path]:
            raise AssertionError("fixture custody changed before unlink")
        function(path)
    shutil.rmtree(directory, onerror=retry)


class BorrowedSource:
    """Return only the enrolled tree; failed recovery forbids ancestor cleanup."""

    def __init__(self, original, spec):
        self.original = M.canonical(original)
        self.spec = spec
        spec.verify_files(self.original, False)
        self.identity = file_identity(self.original)
        self.had_git = (self.original / ".git").exists()
        if self.had_git:
            M.verify_git(self.original, spec)
        self.seal = self.snapshot(self.original)
        self.created_git = None
        self.created_git_seal = None

    @staticmethod
    def snapshot(path):
        # Both passes cap entries; inventory also bounds total bytes before hashing.
        files = M.inventory(path)
        result = {name: (info.st_size, stat.S_IMODE(info.st_mode),
                         M.digest(M.read_file(path / name, info.st_size)))
                  for name, info in files.items()}
        result["./"] = (0, stat.S_IMODE(os.lstat(path).st_mode), "")
        pending, seen, count = [path], set(), 0
        while pending:
            with os.scandir(pending.pop()) as entries:
                for entry in entries:
                    count += 1
                    M.require(count <= M.STORAGE_ENTRIES, "fixture directory entry bound exceeded")
                    name = Path(entry.path).relative_to(path).as_posix()
                    info = os.lstat(entry.path)
                    if M.ordinary(info, True):
                        result[name + "/"] = (0, stat.S_IMODE(info.st_mode), "")
                        pending.append(Path(entry.path))
                    else:
                        M.require(name in files and M.ordinary(info) and info.st_nlink == 1,
                                  "fixture inventory changed during sealing")
                        seen.add(name)
        M.require(seen == set(files), "fixture inventory changed during sealing")
        return result

    def prepare_git(self, current):
        if not self.had_git:
            admin = current / ".git"
            admin.mkdir()
            self.created_git = file_identity(admin)
            install_exact_git(current, self.spec)
            self.created_git_seal = self.snapshot(admin)

    def restore(self, *locations):
        original_error = sys.exc_info()[1]
        try:
            candidates = set((self.original, *locations))
            present = [path for path in candidates if os.path.lexists(path)]
            if len(present) != 1 or file_identity(present[0]) != self.identity:
                raise AssertionError("borrowed tree missing, replaced or destination occupied")
            current = present[0]
            if current != M.canonical(current):
                raise AssertionError("borrowed tree path changed")
            if self.had_git or self.created_git is not None:
                if not self.had_git and self.created_git_seal is None:
                    raise AssertionError("incomplete created Git administration; preserve for recovery")
                M.verify_git(current, self.spec)
                try:
                    self.spec.verify_files(current, False)
                except M.Refusal:
                    # A mixed or partial tree is never treated as a patched tree.
                    self.spec.verify_files(current, True)
                    M.git(current, "apply", "--reverse", "--unidiff-zero", "-p1", "--check",
                          "--", str(self.spec.patch))
                    M.git(current, "apply", "--reverse", "--unidiff-zero", "-p1",
                          "--", str(self.spec.patch))
                self.spec.verify_files(current, False)
                # The complete trusted seal below checks every Git byte after
                # reversal, including created administration before its removal.
                if not self.had_git:
                    admin = current / ".git"
                    if (file_identity(admin) != self.created_git
                            or self.snapshot(admin) != self.created_git_seal):
                        raise AssertionError("created Git administration custody changed")
                    remove_source_fixture(admin, self.created_git, (current,))
                    self.created_git = None
                    self.created_git_seal = None
            self.spec.verify_files(current, False)
            if self.snapshot(current) != self.seal:
                raise AssertionError("borrowed source bytes, modes or Git metadata changed")
            if file_identity(current) != self.identity:
                raise AssertionError("borrowed tree custody changed before return")
            if current != self.original:
                if os.path.lexists(self.original):
                    raise AssertionError("borrowed return destination occupied")
                os.rename(current, self.original)
            if (file_identity(self.original) != self.identity
                    or self.snapshot(self.original) != self.seal):
                raise AssertionError("borrowed source return verification failed")
        except BaseException as error:
            raise AssertionError(
                f"borrowed source recovery failed; preserve {self.original} and {locations}; "
                f"original failure: {original_error!r}; restoration failure: {error!r}") from error


class SourceFiles(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.spec = specification()
        cls.directory = Path(tempfile.mkdtemp(prefix="rmlui-contract-")).resolve(strict=True)
        cls.directory_identity = file_identity(cls.directory)
        cls.workspace = M.Workspace(cls.directory / "build", cls.spec)
        cls.offline = os.environ.get("LUMINUMBRA_RMLUI_TEST_SOURCE")
        cls.borrowed = None
        try:
            cls.workspace.initialize()
            if cls.offline:
                cls.input = M.canonical(cls.offline)
                if cls.input.parent != M.canonical(os.environ["TMPDIR"]):
                    raise AssertionError("offline input must belong directly to the private temporary root")
                cls.borrowed = BorrowedSource(cls.input, cls.spec)
                cls.workspace.stage.parent.mkdir()
                os.rename(cls.input, cls.workspace.stage)
                cls.borrowed.prepare_git(cls.workspace.stage)
            else:
                cls.workspace.download()
                M.git(cls.workspace.source, "apply", "--reverse", "--unidiff-zero", "-p1", "--", str(cls.spec.patch))
                cls.workspace.receipt.unlink()
                os.rename(cls.workspace.source, cls.workspace.stage)
            M.verify_git(cls.workspace.stage, cls.spec)
            cls.spec.verify_files(cls.workspace.stage, False)
        except BaseException:
            if cls.borrowed is not None:
                cls.borrowed.restore(cls.workspace.stage, cls.workspace.source)
            # Without an enrolled offline input this fresh directory is owned.
            # A failed restoration raises above and preserves its ancestor.
            remove_source_fixture(cls.directory, cls.directory_identity,
                                  (cls.workspace.stage, cls.workspace.source))
            raise

    @classmethod
    def tearDownClass(cls):
        if cls.borrowed is not None:
            cls.borrowed.restore(cls.workspace.stage, cls.workspace.source)
        else:
            current = cls.workspace.stage if cls.workspace.stage.exists() else cls.workspace.source
            M.verify_git(current, cls.spec)
            cls.spec.verify_files(current, False)
        remove_source_fixture(cls.directory, cls.directory_identity,
                              (cls.workspace.stage, cls.workspace.source))

    def test_full_pin_publication_and_readonly_repeat(self):
        w = self.workspace
        owner = w.owner()
        before = {n: M.digest(M.read_file(w.stage / n, e["size"])) for n, e in self.spec.files.items()}
        M.write_exclusive(w.admission, b"test admission\n")
        self.assertFalse(w.receipt.exists())
        borrowed = BorrowedSource(w.stage, self.spec)
        try:
            w.publish(owner)
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
            borrowed.restore(w.source)
            if w.receipt.exists():
                w.receipt.unlink()

    def test_every_changed_file_preserves_operations_defaults_and_enum_declarations(self):
        source = self.workspace.stage
        changed = [n for n, e in self.spec.files.items() if e["pristine_sha256"] != e["patched_sha256"]]
        pristine = {n: (source / n).read_bytes() for n in changed}
        M.git(source, "apply", "--unidiff-zero", "-p1", "--", str(self.spec.patch))
        try:
            block = "".join("\tusing " + n + " = LengthPercentageAuto;\n" for n in ("Top", "Right", "Bottom", "Left"))
            for name, before in pristine.items():
                after = (source / name).read_text(encoding="utf-8", errors="strict")
                restored = re.sub(r"\binitial_(\w+)\b", r"\1", after)
                original = before.decode("utf-8", errors="strict")
                if name.endswith("/StyleTypes.h"):
                    self.assertEqual(after.count(block), 1)
                    self.assertGreater(after.index(block), after.index("enum class OriginY"))
                    self.assertEqual(re.findall(r"enum class[^;]+;", original), re.findall(r"enum class[^;]+;", after))
                    restored = restored.replace(block, "")
                    original = original.replace(block, "")
                self.assertEqual(restored, original, name)
        finally:
            M.git(source, "apply", "--reverse", "--unidiff-zero", "-p1", "--", str(self.spec.patch))

    def test_operation_assertions_use_strict_utf8_with_a_cp1252_default(self):
        read = Path.read_text
        def cp1252(path, *args, **kwargs):
            kwargs.setdefault("encoding", "cp1252")
            kwargs.setdefault("errors", "strict")
            return read(path, *args, **kwargs)
        with mock.patch.object(Path, "read_text", cp1252):
            self.test_every_changed_file_preserves_operations_defaults_and_enum_declarations()

    def test_interruption_at_each_publication_boundary_never_marks_incomplete_ready(self):
        w = self.workspace
        real_git, real_rename, real_write = M.git, M.os.rename, M.write_exclusive
        real_unlink = Path.unlink
        for boundary in ("check", "apply", "postverify", "rename", "receipt", "admission"):
            with self.subTest(boundary=boundary):
                M.write_exclusive(w.admission, b"controlled admission\n")
                owner = w.owner()
                borrowed = BorrowedSource(w.stage, self.spec)
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
                    borrowed.restore(w.source)
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
            original_mode = stat.S_IMODE(os.lstat(path).st_mode)
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
            path.chmod(original_mode)
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
            # These fixed probes need at most two administrative parent levels.
            missing = [parent for parent in path.parents
                       if parent != admin and parent.is_relative_to(admin)
                       and not os.path.lexists(parent)]
            created, probe_identity = [], None
            try:
                for parent in reversed(missing):
                    parent.mkdir()
                    created.append((parent, file_identity(parent)))
                with path.open("xb") as stream:
                    info = os.fstat(stream.fileno())
                    probe_identity = info.st_dev, info.st_ino
                    stream.write(b"foreign\n")
                with self.assertRaises(M.Refusal):
                    M.verify_git(source, self.spec)
                self.assertEqual(path.read_bytes(), b"foreign\n")
            finally:
                if probe_identity is not None:
                    M.require(file_identity(path) == probe_identity, "Git fixture probe custody changed")
                    path.unlink()
                for parent, identity in reversed(created):
                    M.require(file_identity(parent) == identity, "Git fixture parent custody changed")
                    parent.rmdir()
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
        original_mode = stat.S_IMODE(os.lstat(path).st_mode)
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
            path.chmod(original_mode)

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


class BorrowedFixtureRecovery(unittest.TestCase):
    setUpClass = classmethod(SourceFiles.setUpClass.__func__)
    tearDownClass = classmethod(SourceFiles.tearDownClass.__func__)

    def test_setup_final_verification_failure_returns_the_complete_input(self):
        original = self.workspace.stage
        identity, seal = file_identity(original), BorrowedSource.snapshot(original)
        for boundary in ("git", "files"):
            with self.subTest(boundary=boundary):
                class Fixture:
                    pass
                real_git, real_files = M.verify_git, M.Specification.verify_files
                failed = False
                def verify_git(path, spec):
                    nonlocal failed
                    real_git(path, spec)
                    if boundary == "git" and path != original and not failed:
                        failed = True
                        raise RuntimeError("injected setup verification")
                def verify_files(spec, path, patched):
                    nonlocal failed
                    real_files(spec, path, patched)
                    if boundary == "files" and path != original and not failed:
                        failed = True
                        raise RuntimeError("injected setup verification")
                with mock.patch.dict(os.environ, LUMINUMBRA_RMLUI_TEST_SOURCE=str(original),
                                     TMPDIR=str(original.parent)), \
                        mock.patch.object(M, "verify_git", side_effect=verify_git), \
                        mock.patch.object(M.Specification, "verify_files", verify_files):
                    with self.assertRaisesRegex(RuntimeError, "injected setup verification"):
                        SourceFiles.setUpClass.__func__(Fixture)
                self.assertTrue(failed)
                self.assertFalse(Fixture.directory.exists())
                self.assertEqual(file_identity(original), identity)
                self.assertEqual(BorrowedSource.snapshot(original), seal)

    def test_teardown_refusal_preserves_ancestor_and_can_retry_both_locations(self):
        original = self.workspace.stage
        for published in (False, True):
            with self.subTest(published=published):
                class Fixture:
                    pass
                with mock.patch.dict(os.environ, LUMINUMBRA_RMLUI_TEST_SOURCE=str(original),
                                     TMPDIR=str(original.parent)):
                    SourceFiles.setUpClass.__func__(Fixture)
                try:
                    if published:
                        M.write_exclusive(Fixture.workspace.admission, b"test admission\n")
                        Fixture.workspace.publish(Fixture.workspace.owner())
                    current = Fixture.workspace.source if published else Fixture.workspace.stage
                    with mock.patch.object(M, "verify_git", side_effect=OSError("injected teardown refusal")):
                        with self.assertRaisesRegex(AssertionError, "restoration failure:.*teardown refusal"):
                            SourceFiles.tearDownClass.__func__(Fixture)
                    self.assertTrue(Fixture.directory.exists())
                    self.assertEqual(file_identity(current), Fixture.borrowed.identity)
                    self.assertFalse(original.exists())
                finally:
                    SourceFiles.tearDownClass.__func__(Fixture)
                self.assertTrue(original.exists())
                self.assertFalse(Fixture.directory.exists())

    def test_borrowed_recovery_refuses_unknown_partial_and_changed_metadata(self):
        original = self.workspace.stage
        borrowed = BorrowedSource(original, self.spec)
        root = Path(tempfile.mkdtemp(prefix="borrowed refusal ")).resolve(strict=True)
        root_identity = file_identity(root)
        try:
            current, hidden = root / "current", root / "hidden"
            os.rename(original, current)
            try:
                # Missing custody and an occupied original destination cannot
                # select or adopt a different tree, including empty input sets.
                os.rename(current, hidden)
                with self.assertRaisesRegex(AssertionError, "missing, replaced or destination occupied"):
                    borrowed.restore(current)
                current.mkdir()
                with self.assertRaisesRegex(AssertionError, "missing, replaced or destination occupied"):
                    borrowed.restore(current)
                current.rmdir()
                os.rename(hidden, current)
                original.mkdir()
                with self.assertRaisesRegex(AssertionError, "destination occupied"):
                    borrowed.restore(current)
                original.rmdir()
                name = next(iter(self.spec.files))
                for path in (current / name, current / ".git/HEAD"):
                    raw = path.read_bytes()
                    path.write_bytes(b"partial")
                    try:
                        with self.assertRaisesRegex(AssertionError, "borrowed source recovery failed"):
                            borrowed.restore(current)
                        self.assertEqual(file_identity(current), borrowed.identity)
                        self.assertEqual(path.read_bytes(), b"partial")
                    finally:
                        path.write_bytes(raw)
                extra = current / ".git/unexpected-empty"
                extra.mkdir()
                try:
                    with self.assertRaisesRegex(AssertionError, "bytes, modes or Git metadata changed"):
                        borrowed.restore(current)
                    self.assertTrue(extra.is_dir())
                    self.assertEqual(file_identity(current), borrowed.identity)
                finally:
                    extra.rmdir()
                with mock.patch.object(M, "read_file", side_effect=PermissionError("injected unreadable input")):
                    with self.assertRaisesRegex(AssertionError, "unreadable input"):
                        borrowed.restore(current)
                with mock.patch.object(os, "rename", side_effect=OSError("injected return failure")):
                    with self.assertRaisesRegex(AssertionError, "return failure"):
                        borrowed.restore(current)
                self.assertEqual(file_identity(current), borrowed.identity)
            finally:
                borrowed.restore(current)
        finally:
            # A restoration exception bypasses recursive allocation cleanup.
            borrowed.restore(root / "current", root / "hidden")
            remove_source_fixture(root, root_identity, ())

        if os.name == "posix":
            # Two fixed unpatched files isolate both unlink/recreate consumers
            # from Git's permission normalization when applying changed files.
            names = (next(iter(self.spec.files)), "LICENSE.txt")
            self.assertTrue(all(self.spec.files[name]["pristine_sha256"]
                                == self.spec.files[name]["patched_sha256"] for name in names))
            paths = [original / name for name in names]
            modes = {path: stat.S_IMODE(os.lstat(path).st_mode) for path in paths}
            try:
                for path, mode in modes.items():
                    variant = mode ^ stat.S_IRGRP
                    self.assertNotEqual(variant, mode)
                    path.chmod(variant)
                SourceFiles.test_nonordinary_files_and_links(self)
                SourceFiles.test_files_mixed_missing_extra_and_partial_refuse_untouched(self)
                for path, mode in modes.items():
                    variant = mode ^ stat.S_IRGRP
                    self.assertEqual(stat.S_IMODE(os.lstat(path).st_mode), variant)
            finally:
                for path, mode in modes.items():
                    path.chmod(mode)

    def test_borrowed_archive_without_git_returns_its_original_metadata(self):
        original = self.workspace.stage
        root = Path(tempfile.mkdtemp(prefix="borrowed Git recovery ")).resolve(strict=True)
        root_identity = file_identity(root)
        try:
            saved_git = root / "original-git"
            os.rename(original / ".git", saved_git)
            try:
                for state in ("unprepared", "prepared", "partial"):
                    with self.subTest(state=state):
                        borrowed = BorrowedSource(original, self.spec)
                        current = root / "current"
                        os.rename(original, current)
                        try:
                            if state == "partial":
                                partial_identity = None
                                def install_failure(path, spec):
                                    nonlocal partial_identity
                                    head = path / ".git/HEAD"
                                    head.write_bytes(b"partial")
                                    partial_identity = file_identity(head)
                                    raise OSError("injected Git installation failure")
                                with mock.patch(__name__ + ".install_exact_git", side_effect=install_failure):
                                    with self.assertRaisesRegex(OSError, "installation failure"):
                                        borrowed.prepare_git(current)
                                with self.assertRaisesRegex(AssertionError, "restoration failure"):
                                    borrowed.restore(current)
                                self.assertTrue(current.exists())
                                head = current / ".git/HEAD"
                                self.assertEqual(head.read_bytes(), b"partial")
                                self.assertEqual(file_identity(head), partial_identity)
                                # Explicitly repair only this injection's own file
                                # and empty directory; the helper never adopts it.
                                head.unlink()
                                self.assertEqual(file_identity(current / ".git"), borrowed.created_git)
                                (current / ".git").rmdir()
                                borrowed.created_git = None
                            elif state == "prepared":
                                borrowed.prepare_git(current)
                                extra = current / ".git/unexpected-empty"
                                extra.mkdir()
                                try:
                                    with self.assertRaisesRegex(AssertionError, "administration custody changed"):
                                        borrowed.restore(current)
                                    self.assertTrue(extra.exists())
                                finally:
                                    extra.rmdir()
                                M.git(current, "apply", "--unidiff-zero", "-p1", "--", str(self.spec.patch))
                                with mock.patch(__name__ + ".remove_source_fixture",
                                                side_effect=PermissionError("injected Git cleanup refusal")):
                                    with self.assertRaisesRegex(AssertionError, "Git cleanup refusal"):
                                        borrowed.restore(current)
                                self.assertTrue((current / ".git").exists())
                                with mock.patch.object(os, "rename", side_effect=OSError("injected return failure")):
                                    with self.assertRaisesRegex(AssertionError, "return failure"):
                                        borrowed.restore(current)
                                self.assertTrue(current.exists())
                                self.assertFalse((current / ".git").exists())
                        finally:
                            borrowed.restore(current)
                        self.assertFalse((original / ".git").exists())
                        self.assertEqual(BorrowedSource.snapshot(original), borrowed.seal)
            finally:
                os.rename(saved_git, original / ".git")
        finally:
            # Keep the saved original Git directory if any recovery step failed.
            M.verify_git(original, self.spec)
            self.spec.verify_files(original, False)
            remove_source_fixture(root, root_identity, ())


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
        cmake = (ROOT / "cmake/rmlui_source.cmake").read_text(encoding="utf-8", errors="strict")
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
        self.assertIn('"tools/ci/test_rmlui_source.py"', (ROOT / "tools/ci/ci_paths.py").read_text(encoding="utf-8", errors="strict"))
        self.assertIn("RmlUiSourceScriptContract", (ROOT / "tools/CMakeLists.txt").read_text(encoding="utf-8", errors="strict"))
        for workflow in ("ci.yml", "asan.yml", "performance.yml", "release.yml"):
            text = (ROOT / ".github/workflows" / workflow).read_text(encoding="utf-8", errors="strict")
            self.assertIn("rmlui_source.py init --root", text)
        self.assertIn("tools/ci/ci_paths.py python-tests", (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8", errors="strict"))

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
                    def fileno(self):
                        if not observed.wait(5):
                            raise AssertionError("storage monitor did not run")
                        return self.stream.fileno()
                    def close(self):
                        return self.stream.close()
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
        self.assertIn('repository / "' + manifest + '"', MODULE_PATH.read_text(encoding="utf-8", errors="strict"))
        self.assertIn('set(_rmlui_manifest "${CMAKE_SOURCE_DIR}/' + manifest + '")',
                      (ROOT / "cmake/rmlui_source.cmake").read_text(encoding="utf-8", errors="strict"))
        result = subprocess.run(["git", "check-attr", "text", "eol", "--", manifest],
                                cwd=ROOT, capture_output=True, text=True, check=True,
                                env=dict(os.environ, GIT_OPTIONAL_LOCKS="0"))
        self.assertEqual(result.stdout.splitlines(),
                         [manifest + ": text: set", manifest + ": eol: lf"])
        self.assertNotIn(b"\r", self.spec.manifest_bytes)

    def test_ucrt_enrollment_failure_stops_configure_and_legacy_tree_remains_usable(self):
        sys.path.insert(0, str(MODULE_PATH.parent))
        from ci_shell import bash_executable
        script = (ROOT / "tools/ci/ucrt_attribution.sh").read_text(encoding="utf-8", errors="strict")
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
                calls = (root / "calls").read_text(encoding="utf-8", errors="strict").splitlines()
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
        workflow = (ROOT / ".github/workflows/performance.yml").read_text(encoding="utf-8", errors="strict")
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
        return [json.loads(line) for line in path.read_text(encoding="utf-8", errors="strict").splitlines()] if path.exists() else []

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


class BootstrapInterpreterBinding(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="rmlui-interpreter-")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve(strict=True)
        for name in ("cmake/rmlui_source.cmake", "tools/ci/rmlui_source.py",
                     "vendor/rmlui/rmlui-6.1-source.json", "cmake/patches/rmlui-6.1-shadow.patch"):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes((ROOT / name).read_bytes())
        (self.root / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.20)\n'
            'project(InterpreterBoundary LANGUAGES NONE)\n'
            'include(FetchContent)\n'
            'set(FETCHCONTENT_FULLY_DISCONNECTED ON)\n'
            'include("${CMAKE_SOURCE_DIR}/cmake/rmlui_source.cmake")\n', encoding="utf-8")

    def initialize(self, build):
        result = subprocess.run([sys.executable, "-I", "-B", str(self.root / "tools/ci/rmlui_source.py"),
                                 "init", "--root", str(build)], capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return (build / ".rmlui-owner.json").read_bytes()

    def configure(self, build, executable):
        args = ["cmake", "-S", str(self.root), "-B", str(build), "-G", "Ninja"]
        if executable is not None:
            args.append("-DPython3_EXECUTABLE=" + executable)
        return subprocess.run(args, capture_output=True, text=True, check=False)

    def test_ucrt_workflow_binds_bootstrap_interpreter_at_real_cmake_boundary(self):
        workflow = (ROOT / ".github/workflows/ci.yml").read_text(encoding="utf-8")
        marker = "      - name: Prepare fresh RmlUi build root\n        shell: msys2 {0}\n"
        section = workflow.split(marker, 1)[1]
        initializer = section.split("run: ", 1)[1].splitlines()[0]
        configure = " ".join(textwrap.dedent(section.split("run: >-\n", 1)[1]
                                             .split("\n      - name:", 1)[0]).splitlines())
        # Model an independent discovery candidate through the preset. The real
        # workflow must replace it with the bootstrap's actual executable.
        (self.root / "CMakePresets.json").write_text(json.dumps({
            "version": 2, "configurePresets": [{"name": "release", "generator": "Ninja",
                "binaryDir": "${sourceDir}/build/release", "cacheVariables": {
                    "Python3_EXECUTABLE": str(self.root / "unselected-python")}}]}), encoding="utf-8")
        sys.path.insert(0, str(MODULE_PATH.parent))
        from ci_shell import bash_executable
        script = ('python3() { "$FIXTURE_PYTHON" "$@"; }\n' + initializer + "\n" + configure)
        result = subprocess.run([bash_executable(), "-e", "-o", "pipefail", "-c", script],
                                cwd=self.root, capture_output=True, text=True, check=False,
                                env=dict(os.environ, FIXTURE_PYTHON=Path(sys.executable).as_posix()))
        output = result.stdout + result.stderr
        self.assertNotEqual(result.returncode, 0)
        # Stop at the real helper's disconnected-delivery refusal: no project,
        # compiler, download, generated native target or upstream CMake runs.
        self.assertIn("fully disconnected source unavailable", output)
        self.assertNotIn("workspace ownership root mismatch", output)
        build = self.root / "build/release"
        owner = (build / ".rmlui-owner.json").read_bytes()
        again = self.configure(build, sys.executable)
        self.assertIn("fully disconnected source unavailable", again.stdout + again.stderr)
        self.assertEqual((build / ".rmlui-owner.json").read_bytes(), owner)
        self.assertFalse((build / ".rmlui-ready.json").exists())

    def test_mixed_writer_spelling_refuses_at_real_cmake_reader_without_mutation(self):
        build = self.root / "build"
        # Exercise the real writer in a subprocess with an alternate pathlib
        # serialization, then the unmodified reader through LANGUAGES NONE.
        # Windows uses the observed slash difference; POSIX models it with a
        # redundant separator. This is not native Windows qualification.
        writer = textwrap.dedent("""\
            import importlib.util
            import os
            from pathlib import Path
            import sys
            loader = importlib.util.spec_from_file_location("source", sys.argv[1])
            module = importlib.util.module_from_spec(loader)
            loader.loader.exec_module(module)
            root = Path(sys.argv[2])
            canonical = module.canonical
            class WriterPath(type(root)):
                def __str__(self):
                    value = super().__str__()
                    if value == str(root):
                        return value.replace(chr(92), "/") if os.name == "nt" else value + "/"
                    return value
            def writer_canonical(raw):
                path = canonical(raw)
                return WriterPath(path) if path == root else path
            module.canonical = writer_canonical
            raise SystemExit(module.main(["init", "--root", str(root)]))
            """)
        initialized = subprocess.run([sys.executable, "-I", "-B", "-c", writer,
                                      str(self.root / "tools/ci/rmlui_source.py"), str(build)],
                                     capture_output=True, text=True, check=False)
        self.assertEqual(initialized.returncode, 0, initialized.stdout + initialized.stderr)
        owner = (build / ".rmlui-owner.json").read_bytes()
        self.assertNotEqual(json.loads(owner)["root"], str(build))
        refused = self.configure(build, sys.executable)
        self.assertNotEqual(refused.returncode, 0)
        self.assertIn("workspace ownership root mismatch", refused.stdout + refused.stderr)
        self.assertEqual((build / ".rmlui-owner.json").read_bytes(), owner)
        self.assertFalse((build / ".rmlui-ready.json").exists())
        self.assertFalse((build / ".rmlui-admission").exists())

    def test_empty_relative_and_missing_interpreter_bindings_refuse_before_helper(self):
        for index, executable in enumerate((None, "", "python3", str(self.root / "missing-python"))):
            with self.subTest(executable=executable):
                build = self.root / ("build-" + str(index))
                owner = self.initialize(build)
                result = self.configure(build, executable)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("RmlUi requires an absolute bootstrap Python3_EXECUTABLE",
                              result.stdout + result.stderr)
                self.assertEqual((build / ".rmlui-owner.json").read_bytes(), owner)
                self.assertFalse((build / ".rmlui-ready.json").exists())

    def test_all_current_configure_callers_bind_the_bootstrap_interpreter(self):
        for name in ("ci.yml", "asan.yml", "performance.yml", "release.yml"):
            source = (ROOT / ".github/workflows" / name).read_text(encoding="utf-8")
            calls = [line for line in source.splitlines() if "cmake " in line and (
                re.search(r"cmake --preset (?:release|perf|debug-asan)\b", line)
                or (" -S " in line and " -B build/" in line))]
            self.assertTrue(calls, name)
            for call in calls:
                with self.subTest(path=name, call=call):
                    self.assertIn("-DPython3_EXECUTABLE=", call)
                    self.assertIn("-I -B -c 'import sys; print(sys.executable)'", call)
        for name in ("README.md", "CONTRIBUTING.md", "docs/development.md", "docs/index.md",
                     "docs/static-morph-assets.md", "docs/performance.md", "docs/known-limitations.md",
                     "docs/static-preview.md", "docs/dependency-source.md", "docs/ci/local-checks.md",
                     "tools/ci/ucrt_attribution.sh", "tools/gates/run-coverage.ps1",
                     "tools/gates/validate-engine-frontier.ps1"):
            loader = importlib.util.spec_from_file_location("binding_paths", ROOT / "tools/ci/ci_paths.py")
            paths = importlib.util.module_from_spec(loader)
            loader.loader.exec_module(paths)
            self.assertTrue(paths.needs_python(name), name)
            source = (ROOT / name).read_text(encoding="utf-8")
            calls = re.findall(r"(?m)^.*cmake (?:--preset (?:[a-z-]+|\$parityPreset)|"
                               r"-B build/debug-fresh --preset debug).*$", source)
            calls = [call for call in calls if not call.lstrip().startswith(('throw ', 'return ', '#'))]
            self.assertTrue(calls, name)
            for call in calls:
                with self.subTest(path=name, call=call):
                    self.assertIn("-DPython3_EXECUTABLE=", call)


class HostedConfigurationContracts(unittest.TestCase):
    def test_git_proxy_spellings_preserve_transport_environment_without_git_injection(self):
        source = ROOT / "synthetic-source"
        executable = ROOT / "synthetic-bin" / "git"
        base = {"Path": str(executable.parent), "SystemRoot": "system-root",
                "ComSpec": "command-shell", "PathExt": ".EXE", "Temp": "temp",
                "Tmp": "tmp", "TmpDir": "tmpdir"}
        inherited = {key: "inherited-control" for key in (
            "HOME", "XDG_CONFIG_HOME", "GIT_CONFIG", "GIT_CONFIG_COUNT",
            "GIT_CONFIG_KEY_0", "GIT_CONFIG_VALUE_0", "GIT_CONFIG_PARAMETERS",
            "GIT_DIR", "GIT_WORK_TREE", "GIT_COMMON_DIR", "GIT_INDEX_FILE",
            "GIT_OBJECT_DIRECTORY", "GIT_ALTERNATE_OBJECT_DIRECTORIES",
            "GIT_EXEC_PATH", "GIT_TEMPLATE_DIR", "GIT_SSH_COMMAND", "GIT_PROXY_COMMAND",
            "GIT_ASKPASS", "SSH_ASKPASS", "GIT_CONFIG_NOSYSTEM", "GIT_CONFIG_GLOBAL",
            "GIT_CONFIG_SYSTEM", "GIT_ATTR_NOSYSTEM", "GIT_NO_REPLACE_OBJECTS",
            "GIT_OPTIONAL_LOCKS", "GIT_TERMINAL_PROMPT", "GIT_CONFIG_SYSTEM_EXTRA",
            "https_proxy_extra", "core.hooksPath", "filter.fixture.clean", "credential.helper")}
        forced = {"GIT_CONFIG_NOSYSTEM": "1", "GIT_CONFIG_GLOBAL": os.devnull,
                  "GIT_CONFIG_SYSTEM": os.devnull, "GIT_ATTR_NOSYSTEM": "1",
                  "GIT_NO_REPLACE_OBJECTS": "1", "GIT_OPTIONAL_LOCKS": "0",
                  "GIT_TERMINAL_PROMPT": "0"}
        proxies = {key: "http://proxy.invalid:8123" for key in (
            "http_proxy", "HTTP_PROXY", "Http_Proxy", "https_proxy", "HTTPS_PROXY",
            "Https_Proxy", "all_proxy", "ALL_PROXY", "All_Proxy")}
        proxies.update(no_proxy="local.invalid", NO_PROXY="upper.invalid", No_Proxy="")
        expected_argv = [str(executable.resolve()), "-c", "core.autocrlf=false",
                         "-c", "core.safecrlf=false", "-c", "core.hooksPath=" + os.devnull,
                         "-c", "core.fsmonitor=false", "-c", "protocol.file.allow=never",
                         "-C", str(source), "fetch", "--depth=1", "https://source.invalid", "revision"]
        output = b"captured-output"
        for proxy_environment in ({}, proxies):
            for payload in (None, b"request-bytes"):
                with self.subTest(proxies=bool(proxy_environment), payload=payload):
                    # A plain synthetic mapping also models original spellings on
                    # hosts whose native environment mapping folds key casing.
                    environment = dict(base, **inherited, **proxy_environment)
                    with mock.patch.object(M.os, "environ", environment), \
                            mock.patch.object(M.shutil, "which", return_value=str(executable)) as which, \
                            mock.patch.object(M, "run_command", return_value=output) as command, \
                            mock.patch.object(M.subprocess, "Popen", side_effect=AssertionError("unexpected process")):
                        result = M.git(source, "fetch", "--depth=1", "https://source.invalid", "revision",
                                       input_bytes=payload)
                    self.assertIs(result, output)
                    which.assert_called_once_with("git", path=base["Path"])
                    expected = {key.upper(): value for key, value in base.items()}
                    expected.update(forced)
                    expected.update(proxy_environment)
                    command.assert_called_once_with(source, expected_argv, expected, payload)

    def test_dependency_configuration_keeps_the_original_shared_option_order(self):
        vendor = (ROOT / "vendor/CMakeLists.txt").read_text(encoding="utf-8", errors="strict")
        module = (ROOT / "cmake/rmlui_source.cmake").read_text(encoding="utf-8", errors="strict")
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
                    if "directory" in Fixture.__dict__ and Fixture.directory.exists():
                        remove_source_fixture(Fixture.directory, Fixture.directory_identity,
                                              (Fixture.workspace.stage, Fixture.workspace.source))


class OwnedCommandLifetime(unittest.TestCase):
    def setUp(self):
        self.command_threads = []
        self.test_jobs = []
        self.test_processes = []
        self.custody_lock = threading.Lock()
        self.custody_stopping = False
        self.custody_closed = False
        self.launched = []
        self.directory = tempfile.mkdtemp(prefix="rmlui-process-")
        self.root = Path(self.directory).resolve(strict=True)
        self.fixture_identity = file_identity(self.root)
        self.addCleanup(self.retire_command_fixture)
        self.addCleanup(self.stop_sessions)
        self.script = self.root / "command.py"
        self.script.write_text(textwrap.dedent("""\
            import os
            from pathlib import Path
            import subprocess
            import sys
            import time
            root = Path(sys.argv[1])
            mode = sys.argv[2]
            if mode == "child":
                mode = sys.argv[3]
                (root / "child.pid").write_text(str(os.getpid()), encoding="ascii")
                with (root / "work").open("ab", buffering=0) as work:
                    if mode == "storage":
                        entries = root / "entries"
                        entries.mkdir()
                        for i in range(8193):
                            (entries / str(i)).touch()
                        (root / "entries-complete").touch()
                    while True:
                        work.write(b"x")
                        if mode == "output":
                            try:
                                os.write(1, b"x" * 65536)
                            except OSError:
                                mode = "closed-output"
                        time.sleep(0.005)
            elif mode in ("storage", "output", "exited", "input"):
                subprocess.Popen([sys.executable, "-I", "-B", __file__, str(root), "child", mode])
                limit = time.monotonic() + 3
                while not (root / "work").exists():
                    if time.monotonic() > limit:
                        raise RuntimeError("child did not start")
                    time.sleep(0.005)
                if mode in ("exited", "input"):
                    sys.exit(0)
                time.sleep(20)
            elif mode == "startup":
                with (root / "work").open("ab", buffering=0) as work:
                    while True:
                        work.write(b"x")
                        time.sleep(0.005)
            elif mode == "broken-input":
                os.close(0)
                (root / "closed").touch()
                time.sleep(0.2)
            elif mode == "nonzero":
                sys.exit(7)
            else:
                os.write(1, "strict utf8: ö\\n".encode("utf-8"))
            """), encoding="utf-8")
        self.launched = []
        self.process = subprocess.Popen
        if os.name == "posix":
            class RetainedProcess(subprocess.Popen):
                # Keep the private session leader unreaped until test cleanup,
                # including when exercising the old direct-parent-only code.
                def poll(self):
                    info = os.waitid(os.P_PID, self.pid, os.WEXITED | os.WNOHANG | os.WNOWAIT)
                    if info is None:
                        return None
                    return info.si_status if info.si_code == os.CLD_EXITED else -info.si_status
                def wait(self, timeout=None):
                    deadline = None if timeout is None else time.monotonic() + timeout
                    while True:
                        status = self.poll()
                        if status is not None:
                            self.returncode = status
                            return status
                        if deadline is not None and time.monotonic() >= deadline:
                            raise subprocess.TimeoutExpired(self.args, timeout)
                        time.sleep(0.005)
            self.process = RetainedProcess
        process_factory = self.process
        def retained_process(argv, **kwargs):
            self.assertTrue(self.custody_lock.acquire(timeout=2), "launch custody deadline exceeded")
            try:
                self.assertFalse(self.custody_stopping, "test process custody is closing")
                if os.name == "nt" and "--contained-git" in argv:
                    name = argv[argv.index("--contained-git") + 1]
                    # Retain query/termination custody before the bootstrap can run.
                    # This handle belongs only to the test; production stays real.
                    job = M.WindowsJob(name, existing=True)
                    handle = job.api.OpenJobObjectW(0x0004 | 0x0008, False, name)
                    if not handle:
                        error = job.ctypes.WinError(job.ctypes.get_last_error())
                        job.close()
                        raise error
                    job.close()
                    job.handle = handle
                    self.test_jobs.append(job)
                process = process_factory(argv, **kwargs)
                # Sibling refusal fixtures replace these methods. Retain the
                # actual methods before returning the process to any caller.
                self.test_processes.append((process, process.kill, process.wait))
                return process
            finally:
                self.custody_lock.release()
        self.process = retained_process
        self.real_popen = subprocess.Popen

    def retire_command_fixture(self):
        self.assertTrue(self.custody_closed, "fixture shutdown remains unverified")
        self.assertEqual(file_identity(self.root), self.fixture_identity)
        shutil.rmtree(self.root)

    def stop_command_tree(self):
        self.assertTrue(self.custody_lock.acquire(timeout=2), "shutdown custody deadline exceeded")
        try:
            self.custody_stopping = True
            processes, jobs = tuple(self.test_processes), tuple(self.test_jobs)
        finally:
            self.custody_lock.release()
        if os.name == "nt":
            # A live bootstrap may not have enrolled yet. Observe its death
            # before terminating and observing the exact retained job empty.
            for process, kill, wait in processes:
                kill()
                wait(timeout=2)
            for job in jobs:
                job.terminate()
                job.wait_empty()
        else:
            import signal
            for process, kill, wait in processes:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass

    def stop_sessions(self):
        self.stop_command_tree()
        for thread in self.command_threads:
            thread.join(2)
            self.assertFalse(thread.is_alive(), "owned command cleanup did not join")
        for job in self.test_jobs:
            job.close()
        self.test_jobs.clear()

        if os.name == "posix":
            import signal
            for process in self.launched:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            for process in self.launched:
                deadline = time.monotonic() + 2
                try:
                    while True:
                        waited, status = os.waitpid(process.pid, os.WNOHANG)
                        if waited:
                            process.returncode = os.waitstatus_to_exitcode(status)
                            break
                        if time.monotonic() >= deadline:
                            raise AssertionError("test session cleanup deadline exceeded")
                        time.sleep(0.005)
                except ChildProcessError:
                    pass
            self.launched.clear()
        self.custody_closed = True

    def command_diagnostics(self, thread):
        # One snapshot, 16 frames, four retained launch records and three known
        # fixture paths only. Never inspect locals, scan storage or poll/wait children.
        frame = sys._current_frames().get(thread.ident)
        stack = []
        for _ in range(16):
            if frame is None:
                break
            stack.append(dict(file=Path(frame.f_code.co_filename).name,
                              function=frame.f_code.co_name, line=frame.f_lineno))
            frame = frame.f_back
        details = dict(stack=stack, stack_truncated=frame is not None,
                       launches=[dict(pid=p.pid, returncode=p.returncode) for p in self.launched[-4:]])
        del frame
        for name in ("work", "child.pid", "entries-complete"):
            try:
                info = os.lstat(self.root / name)
                details[name] = dict(size=info.st_size, mode=info.st_mode)
                if name == "child.pid":
                    details[name]["pid"] = int(M.read_file(self.root / name, 32))
            except (OSError, ValueError) as error:
                details[name] = dict(unavailable=type(error).__name__)
        return details

    def command(self, mode, input_bytes=None):
        def launch(argv, **kwargs):
            actual = [sys.executable, "-I", "-B", str(self.script), str(self.root), mode]
            if "--contained-git" in argv:
                offset = argv.index("--contained-git") + 2
                actual = argv[:offset] + actual
            # Test custody also contains a reverted implementation's leaked
            # children, so demonstrating the old hang cannot strand a writer.
            if os.name == "posix":
                kwargs["start_new_session"] = True
            result = self.process(actual, **kwargs)
            self.launched.append(result)
            if mode in ("input", "broken-input") and os.name == "posix":
                import fcntl
                if hasattr(fcntl, "F_SETPIPE_SZ"):
                    fcntl.fcntl(result.stdin.fileno(), fcntl.F_SETPIPE_SZ, 4096)
            if mode == "broken-input":
                deadline = time.monotonic() + 3
                while not (self.root / "closed").exists():
                    if time.monotonic() > deadline:
                        raise AssertionError("input closure not reached")
                    time.sleep(0.005)
            return result
        result, errors = [], []
        def run():
            try:
                with mock.patch.object(M.subprocess, "Popen", side_effect=launch):
                    result.append(M.git(self.root, "--version", input_bytes=input_bytes))
            except BaseException as error:
                errors.append(error)
        thread = threading.Thread(target=run, daemon=True)
        self.command_threads.append(thread)
        started = time.monotonic()
        thread.start()
        thread.join(4)
        try:
            if thread.is_alive():
                print("owned command deadline: " + json.dumps(self.command_diagnostics(thread),
                                                              sort_keys=True), file=sys.stderr)
            self.assertFalse(thread.is_alive(), "owned command did not return within four seconds")
            self.assertLess(time.monotonic() - started, 4)
            if errors:
                raise errors[0]
            return result[0]
        finally:
            # Retire the exact tree before releasing any diagnostic barrier
            # through join(). No writer may survive into fixture removal.
            if thread.is_alive():
                self.stop_command_tree()
            thread.join(2)
            self.assertFalse(thread.is_alive(), "owned command cleanup did not join")

    def assert_child_stopped(self):
        pid = int((self.root / "child.pid").read_text(encoding="ascii"))
        before = (self.root / "work").stat().st_size
        time.sleep(0.12)
        self.assertEqual(before, (self.root / "work").stat().st_size, "descendant continued work")
        if os.name == "posix":
            status = Path("/proc") / str(pid) / "stat"
            if status.exists():
                self.assertEqual(status.read_text(encoding="ascii").split(") ", 1)[1][0], "Z")
        else:
            import ctypes
            from ctypes import wintypes
            api = ctypes.WinDLL("kernel32", use_last_error=True)
            api.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
            api.OpenProcess.restype = wintypes.HANDLE
            api.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
            api.WaitForSingleObject.restype = wintypes.DWORD
            api.CloseHandle.argtypes = [wintypes.HANDLE]
            handle = api.OpenProcess(0x00100000, False, pid)
            if handle:
                try:
                    self.assertEqual(api.WaitForSingleObject(handle, 0), 0)
                finally:
                    api.CloseHandle(handle)
            else:
                self.assertEqual(ctypes.get_last_error(), 87)


    def pipe_exit_after_empty_read(self, final_output, returncode=0):
        """Synchronize at the real pipe boundary, without replacing run_command."""
        self.script.write_text(textwrap.dedent("""\
            import os
            from pathlib import Path
            import sys
            import time
            root = Path(sys.argv[1])
            (root / "ready").touch()
            deadline = time.monotonic() + 3
            while not (root / "release").exists():
                if time.monotonic() >= deadline:
                    raise RuntimeError("release deadline exceeded")
                time.sleep(0.005)
            os.write(1, bytes.fromhex(sys.argv[2]))
            sys.exit(int(sys.argv[3]))
            """), encoding="utf-8")
        observed = []
        original_read = os.read
        original_available = M.WindowsJob.available
        def wait_until(predicate):
            deadline = time.monotonic() + 3
            while not predicate():
                if time.monotonic() >= deadline:
                    raise AssertionError("pipe synchronization deadline exceeded")
                time.sleep(0.005)
        def launch(argv, **kwargs):
            process = self.process(argv, **kwargs)
            self.launched.append(process)
            wait_until(lambda: (self.root / "ready").exists())
            return process
        def release_after_empty():
            process = self.launched[0]
            self.assertIsNone(process.poll(), "empty read must precede leader exit")
            (self.root / "release").touch()
            # POSIX test poll uses WNOWAIT, preserving the owned session identity.
            wait_until(lambda: process.poll() is not None)
            self.assertEqual(process.poll(), returncode)
            observed.append(True)
        def read(fd, count):
            try:
                return original_read(fd, count)
            except BlockingIOError:
                if not observed and self.launched and fd == self.launched[0].stdout.fileno():
                    release_after_empty()
                raise
        def available(job, stream):
            count = original_available(job, stream)
            if count == 0 and not observed:
                release_after_empty()
            return count
        argv = [sys.executable, "-I", "-B", str(self.script), str(self.root),
                final_output.hex(), str(returncode)]
        try:
            with mock.patch.object(M.subprocess, "Popen", side_effect=launch), \
                    mock.patch.object(M.os, "read", side_effect=read), \
                    mock.patch.object(M.WindowsJob, "available", available):
                return M.run_command(self.root, argv, dict(os.environ))
        finally:
            self.assertEqual(observed, [True], "real empty pipe boundary was not exercised")
            self.assertIsNotNone(self.launched[0].poll())
            self.assertFalse((self.root / "child.pid").exists(), "fixture must have no descendant")

    def test_pipe_exit_after_empty_read_preserves_success(self):
        self.assertEqual(self.pipe_exit_after_empty_read(b""), b"")

    def test_pipe_exit_after_empty_read_preserves_final_output(self):
        data = "final utf8: ö\n".encode("utf-8")
        self.assertEqual(self.pipe_exit_after_empty_read(data), data)

    def test_pipe_exit_after_empty_read_preserves_nonzero_refusal(self):
        with self.assertRaisesRegex(M.Refusal, "Git command refused\nfinal failure"):
            self.pipe_exit_after_empty_read(b"final failure", 7)

    def test_deadline_diagnostics_capture_the_actual_owned_parent_stack(self):
        entered, release = threading.Event(), threading.Event()
        real_inventory, real_join = M.inventory, threading.Thread.join
        def held_inventory(source):
            entered.set()
            if not release.wait(3):
                raise AssertionError("diagnostic barrier deadline exceeded")
            return real_inventory(source)
        def join(thread, timeout=None):
            if timeout == 4:
                self.assertTrue(entered.wait(3))
                return
            release.set()
            return real_join(thread, timeout)
        output = io.StringIO()
        try:
            with mock.patch.object(M, "inventory", side_effect=held_inventory),                     mock.patch.object(threading.Thread, "join", join), contextlib.redirect_stderr(output):
                with self.assertRaisesRegex(AssertionError, "owned command did not return within four seconds"):
                    self.command("storage")
        finally:
            release.set()
        self.assertIn("owned command deadline: ", output.getvalue())
        details = json.loads(output.getvalue().split("owned command deadline: ", 1)[1])
        self.assertIn("held_inventory", [frame["function"] for frame in details["stack"]])
        self.assertTrue(all(set(frame) == {"file", "function", "line"} for frame in details["stack"]))
        self.assertLessEqual(len(details["stack"]), 16)
        self.assertEqual(details["launches"][0]["pid"], self.launched[0].pid)
        self.assertIn("work", details)
        self.assertIn("entries-complete", details)
        self.assertTrue(all(not thread.is_alive() for thread in self.command_threads))
        self.assertTrue(all(process.poll() is not None for process in self.launched))
        before = (self.root / "work").stat().st_size if (self.root / "work").exists() else None
        time.sleep(0.12)
        self.assertEqual((self.root / "work").stat().st_size if before is not None else None, before)
        self.assertIs(M.subprocess.Popen, self.real_popen)
        retained = tuple(self.test_processes)
        with self.assertRaisesRegex(AssertionError, "custody is closing"):
            self.process([sys.executable, "-I", "-B", "-c", "pass"])
        self.assertEqual(tuple(self.test_processes), retained)
        with self.assertRaisesRegex(AssertionError, "shutdown remains unverified"):
            self.retire_command_fixture()
        self.assertEqual(file_identity(self.root), self.fixture_identity)
        # Final cleanup must use the original methods, including on Windows.
        for process, kill, wait in retained:
            process.kill = mock.Mock(side_effect=AssertionError("replaced kill must not run"))
            process.wait = mock.Mock(side_effect=AssertionError("replaced wait must not run"))
        self.stop_command_tree()

    def test_deadline_diagnostics_report_missing_progress_without_waiting(self):
        details = self.command_diagnostics(threading.current_thread())
        self.assertEqual(details["launches"], [])
        with mock.patch.object(sys, "_current_frames", return_value={}):
            absent = self.command_diagnostics(threading.current_thread())
        self.assertEqual(absent["stack"], [])
        self.assertFalse(absent["stack_truncated"])
        def nested(depth):
            if depth:
                return nested(depth - 1)
            return self.command_diagnostics(threading.current_thread())
        truncated = nested(17)
        self.assertEqual(len(truncated["stack"]), 16)
        self.assertTrue(truncated["stack_truncated"])
        for name in ("work", "child.pid", "entries-complete"):
            self.assertEqual(details[name], {"unavailable": "FileNotFoundError"})
        with mock.patch.object(M, "read_file", return_value=b"partial"):
            (self.root / "child.pid").touch()
            self.assertEqual(self.command_diagnostics(threading.current_thread())["child.pid"],
                             {"unavailable": "ValueError"})

    def test_storage_refusal_stops_actual_descendant_and_held_pipe(self):
        with self.assertRaisesRegex(M.Refusal, "entry bound"):
            self.command("storage")
        self.assert_child_stopped()

    def test_output_refusal_stops_actual_descendant_and_held_pipe(self):
        with self.assertRaisesRegex(M.Refusal, "output bound"):
            self.command("output")
        self.assert_child_stopped()

    def test_exited_parent_with_descendant_output_refuses_and_stops_work(self):
        with self.assertRaisesRegex(M.Refusal, "descendant retained output"):
            self.command("exited")
        self.assert_child_stopped()

    def test_blocked_input_writer_joins_after_descendant_shutdown(self):
        with self.assertRaises(M.Refusal):
            self.command("input", b"x" * 22655)
        self.assert_child_stopped()

    def test_input_failure_nonzero_success_and_launch_failure_are_bounded(self):
        with self.assertRaises(M.Refusal):
            self.command("broken-input", b"x" * 22655)
        with self.assertRaises(M.Refusal):
            self.command("nonzero")
        self.assertEqual(self.command("success"), "strict utf8: ö\n".encode("utf-8"))
        with mock.patch.object(M.subprocess, "Popen", side_effect=OSError("launch refused")):
            with self.assertRaisesRegex(OSError, "launch refused"):
                M.git(self.root, "--version")

    def test_input_thread_start_failure_terminates_the_already_launched_command(self):
        def launch(argv, **kwargs):
            actual = [sys.executable, "-I", "-B", str(self.script), str(self.root), "startup"]
            if "--contained-git" in argv:
                actual = argv[:argv.index("--contained-git") + 2] + actual
            if os.name == "posix":
                kwargs["start_new_session"] = True
            process = self.process(actual, **kwargs)
            self.launched.append(process)
            deadline = time.monotonic() + 3
            while not (self.root / "work").exists():
                if time.monotonic() >= deadline:
                    raise AssertionError("writer startup fixture did not start")
                time.sleep(0.005)
            return process
        with mock.patch.object(M.subprocess, "Popen", side_effect=launch), \
                mock.patch.object(threading.Thread, "start", side_effect=RuntimeError("thread start refused")):
            with self.assertRaisesRegex(RuntimeError, "thread start refused"):
                M.git(self.root, "--version", input_bytes=b"input")
        before = (self.root / "work").stat().st_size
        time.sleep(0.12)
        self.assertEqual((self.root / "work").stat().st_size, before)
        self.assertIsNotNone(self.launched[0].poll())

    def test_command_deadline_and_excess_input_refuse_without_live_writer(self):
        with mock.patch.object(M.time, "monotonic", side_effect=itertools.count(0, 121)):
            with self.assertRaisesRegex(M.Refusal, "deadline"):
                M.run_command(self.root, [sys.executable, "-I", "-B", "-c", "import time; time.sleep(20)"],
                              dict(os.environ))
        with mock.patch.object(M.subprocess, "Popen", side_effect=AssertionError("must not launch")):
            with self.assertRaisesRegex(M.Refusal, "input bound"):
                M.git(self.root, "--version", input_bytes=b"x" * 22656)


class WindowsContainmentRefusal(unittest.TestCase):
    stop_command_tree = OwnedCommandLifetime.stop_command_tree
    stop_sessions = OwnedCommandLifetime.stop_sessions
    retire_command_fixture = OwnedCommandLifetime.retire_command_fixture

    def assert_child_stopped(self):
        for api, handle in self.child_handles:
            self.assertEqual(api.WaitForSingleObject(handle, 2000), 0)
        OwnedCommandLifetime.assert_child_stopped(self)

    def retain_native_child(self):
        if os.name == "nt":
            import ctypes
            from ctypes import wintypes
            api = ctypes.WinDLL("kernel32", use_last_error=True)
            api.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
            api.OpenProcess.restype = wintypes.HANDLE
            api.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
            api.WaitForSingleObject.restype = wintypes.DWORD
            api.CloseHandle.argtypes = [wintypes.HANDLE]
            api.CloseHandle.restype = wintypes.BOOL
            # The worker is alive behind the spawn barrier in our private job.
            # Retain its synchronization handle before cleanup can terminate it.
            pid = int((self.root / "child.pid").read_text(encoding="ascii"))
            handle = api.OpenProcess(0x00100000, False, pid)
            if not handle:
                raise ctypes.WinError(ctypes.get_last_error())
            self.child_handles.append((api, handle))

    def setUp(self):
        OwnedCommandLifetime.setUp(self)
        self.events = []
        self.jobs = []
        self.owned_processes = []
        self.child_handles = []
        self.writers = []
        self.release_writer = threading.Event()
        self.drained = bytearray()
        self.script.write_text(textwrap.dedent("""\
            import importlib.util
            import os
            from pathlib import Path
            import subprocess
            import sys
            import time

            root, helper, name, phase, has_input = sys.argv[1:]
            root = Path(root)
            def wait_for(marker):
                deadline = time.monotonic() + 3
                while not (root / marker).exists():
                    if time.monotonic() >= deadline:
                        raise RuntimeError("bootstrap barrier deadline exceeded: " + marker)
                    time.sleep(0.005)
            if phase == "worker":
                (root / "child.pid").write_text(str(os.getpid()), encoding="ascii")
                with (root / "work").open("ab", buffering=0) as work:
                    while True:
                        work.write(b"x")
                        time.sleep(0.005)
            (root / "before").touch()
            if phase in ("before", "assignment"):
                wait_for("enroll")
            if os.name == "nt":
                loader = importlib.util.spec_from_file_location("bootstrap_helper", helper)
                module = importlib.util.module_from_spec(loader)
                loader.loader.exec_module(module)
                job = module.WindowsJob(name, existing=True)
                try:
                    job.enroll_bootstrap()
                finally:
                    job.close()
            # These are child-side barriers around actual enrollment on Windows.
            # On POSIX they control only the portable lifetime-order model.
            os.write(1, b"queued output\\n")
            if has_input == "yes":
                if os.read(0, 1) != b"x":
                    raise RuntimeError("bootstrap did not receive the input")
                (root / "input-seen").touch()
            (root / "assigned").touch()
            if phase == "spawn":
                wait_for("spawn")
                subprocess.Popen([sys.executable, "-I", "-B", __file__,
                                  str(root), helper, name, "worker", "no"])
                wait_for("work")
                (root / "spawned").touch()
            wait_for("hold")
            """), encoding="utf-8")
        self.addCleanup(self.cleanup_bootstraps)

    def wait_for(self, marker):
        deadline = time.monotonic() + 3
        while not (self.root / marker).exists():
            if time.monotonic() >= deadline:
                raise AssertionError("child barrier not reached: " + marker)
            time.sleep(0.005)

    def cleanup_bootstraps(self):
        # Exact retained handles only; POSIX session identities remain unreaped
        # until stop_sessions, including while exercising the old implementation.
        for process, kill, wait in self.owned_processes:
            kill()
            wait(timeout=2)
        for job in self.jobs:
            job.finish()
        self.release_writer.set()
        for writer in self.writers:
            threading.Thread.join(writer, timeout=2)
            self.assertFalse(writer.is_alive(), "fixture writer did not stop")
        for api, handle in self.child_handles:
            self.assertEqual(api.WaitForSingleObject(handle, 2000), 0)
            self.assertTrue(api.CloseHandle(handle))
        self.child_handles.clear()

    def run_bootstrap(self, phase, *, errors=(), input_bytes=None, live_writer=False,
                      launch_failure=False, nonzero=False):
        """Real child/pipe ordering; only Windows runs the native job APIs."""
        native = os.name == "nt"
        real_job, real_read, real_thread = M.WindowsJob, os.read, threading.Thread
        owner = self
        platform = mock.Mock(wraps=os)
        platform.name = "nt"
        class Job:
            def __init__(self, name):
                self.real = real_job(name) if native else None
                owner.jobs.append(self)
            def terminate(self):
                owner.events.append("terminate-job")
                if owner.owned_processes and not {"kill", "wait", "kill-denied", "wait-unobserved"}.intersection(errors):
                    owner.assertIn("bootstrap-exited", owner.events)
                    owner.assertIsNotNone(owner.owned_processes[0][0].poll())
                if "terminate-denied" in errors:
                    raise PermissionError("controlled job termination denied")
                if self.real is not None:
                    self.real.terminate()
                elif owner.launched:
                    import signal
                    os.killpg(owner.launched[0].pid, signal.SIGKILL)
                if "terminate" in errors:
                    raise OSError("controlled job termination failure")
            def wait_empty(self):
                owner.events.append("wait-empty")
                if self.real is not None:
                    self.real.wait_empty()
                elif "terminate-denied" in errors:
                    # The portable model has no native active-process query.
                    raise M.Refusal("portable job emptiness remains unverified")
                if "empty" in errors:
                    raise OSError("controlled job emptiness failure")
            def available(self, stream):
                if self.real is not None:
                    return self.real.available(stream)
                import select
                return 65536 if select.select([stream.fileno()], [], [], 0)[0] else 0
            def close(self):
                owner.events.append("close-job")
                self.finish()
                if "close-job" in errors:
                    raise OSError("controlled job close failure")
            def finish(self):
                if self.real is not None:
                    self.real.close()
                elif owner.launched:
                    import signal
                    os.killpg(owner.launched[0].pid, signal.SIGKILL)
        class Pipe:
            def __init__(self, stream, kind):
                self.stream, self.kind = stream, kind
            def __enter__(self):
                return self
            def __exit__(self, *args):
                self.close()
            def fileno(self):
                return self.stream.fileno()
            def write(self, data):
                return self.stream.write(data)
            def close(self):
                if self.kind == "input" and owner.writers:
                    writer = owner.writers[0]
                    if threading.current_thread() is not writer:
                        owner.assertFalse(writer.is_alive(), "stdin closed under live writer")
                owner.events.append("close-" + self.kind)
                self.stream.close()
                if "close-" + self.kind in errors:
                    raise OSError("controlled " + self.kind + " close failure")
        class Writer(real_thread):
            def run(self):
                if live_writer:
                    if not owner.release_writer.wait(3):
                        raise AssertionError("writer release deadline exceeded")
                super().run()
            def join(self, timeout=None):
                owner.events.append("join-writer")
                if "join" in errors:
                    raise OSError("controlled writer join failure")
                super().join(timeout)
        def writer(*args, **kwargs):
            thread = Writer(*args, **kwargs)
            owner.writers.append(thread)
            return thread
        def launch(argv, **kwargs):
            if launch_failure:
                raise OSError("controlled bootstrap launch failure")
            name = argv[argv.index("--contained-git") + 1]
            actual = [sys.executable, "-I", "-B", "-S", str(self.script),
                      str(self.root), str(MODULE_PATH.resolve()), name, phase,
                      "yes" if input_bytes is not None and not live_writer else "no"]
            if nonzero:
                actual = [sys.executable, "-I", "-B", "-c",
                          "import os; os.write(1, b'final failure'); raise SystemExit(7)"]
            kwargs["start_new_session"] = not native
            process = self.process(actual, **kwargs)
            self.launched.append(process)
            kill, wait = process.kill, process.wait
            self.owned_processes.append((process, kill, wait))
            def stop():
                self.events.append("kill-bootstrap")
                if phase == "assignment":
                    (self.root / "enroll").touch()
                    self.wait_for("assigned")
                elif phase == "spawn":
                    (self.root / "spawn").touch()
                    self.wait_for("spawned")
                    self.retain_native_child()
                if "kill-denied" in errors:
                    raise PermissionError("controlled bootstrap kill denied")
                kill()
                if "kill" in errors:
                    raise OSError("controlled bootstrap kill failure")
            def reap(timeout=None):
                self.events.append("wait-bootstrap")
                if "wait-unobserved" in errors:
                    raise OSError("controlled bootstrap exit unobserved")
                result = wait(timeout)
                self.events.append("bootstrap-exited")
                if "wait" in errors:
                    raise OSError("controlled bootstrap reap failure")
                return result
            process.kill, process.wait = stop, reap
            if process.stdin is not None:
                process.stdin = Pipe(process.stdin, "input")
                if not native:
                    import fcntl
                    if hasattr(fcntl, "F_SETPIPE_SZ"):
                        fcntl.fcntl(process.stdin.fileno(), fcntl.F_SETPIPE_SZ, 4096)
            process.stdout = Pipe(process.stdout, "output")
            return process
        def scan(source):
            self.wait_for("assigned" if phase == "spawn" else "before")
            if input_bytes is not None and not live_writer:
                # The child consumed a real byte, then held the remaining input.
                # Its pipe is smaller than the unchanged 22,655-byte payload.
                self.wait_for("input-seen")
                self.assertTrue(self.writers[0].is_alive(), "input writer was not blocked")
                self.assertNotIn("close-input", self.events)
            raise M.Refusal("controlled source refusal")
        def read(fd, count):
            data = real_read(fd, count)
            if self.launched and fd == self.launched[0].stdout.fileno():
                # Capture at most both 4 MiB budgets and their final 64 KiB reads.
                self.assertLessEqual(len(self.drained) + len(data), 2 * M.OUTPUT_BYTES + 2 * 65536)
                self.drained.extend(data)
            return data
        with mock.patch.object(M, "os", platform), \
                mock.patch.object(M, "WindowsJob", Job), \
                mock.patch.object(M.subprocess, "Popen", side_effect=launch), \
                mock.patch.object(M.threading, "Thread", side_effect=writer), \
                mock.patch.object(platform, "read", side_effect=read), \
                mock.patch.object(M, "inventory", side_effect=scan if not nonzero else lambda source: {}):
            return M.run_command(self.root, [sys.executable, "-I", "-B", "-c", "pass"],
                                 dict(os.environ), input_bytes)

    def assert_shutdown_order(self):
        for earlier, later in (("kill-bootstrap", "wait-bootstrap"),
                               ("bootstrap-exited", "terminate-job"),
                               ("terminate-job", "wait-empty"),
                               ("wait-empty", "close-output"), ("close-output", "close-job")):
            self.assertLess(self.events.index(earlier), self.events.index(later), self.events)

    def test_pre_enrollment_refusal_stops_exact_bootstrap_before_final_job(self):
        with self.assertRaisesRegex(M.Refusal, "controlled source refusal"):
            self.run_bootstrap("before")
        self.assert_shutdown_order()
        (self.root / "enroll").touch()
        self.assertFalse((self.root / "assigned").exists())
        self.assertFalse((self.root / "spawned").exists())

    def test_assignment_during_cleanup_cannot_follow_final_job_termination(self):
        with self.assertRaisesRegex(M.Refusal, "controlled source refusal"):
            self.run_bootstrap("assignment")
        self.assertTrue((self.root / "assigned").exists())
        self.assert_shutdown_order()
        self.assertEqual(self.drained, b"queued output\n")

    def test_spawn_during_cleanup_stops_blocked_input_and_drains_held_output(self):
        with self.assertRaisesRegex(M.Refusal, "controlled source refusal"):
            self.run_bootstrap("spawn", input_bytes=b"x" * 22655)
        self.assertTrue((self.root / "spawned").exists())
        self.assert_shutdown_order()
        self.assertLess(self.events.index("wait-empty"), self.events.index("join-writer"))
        self.assertFalse(self.writers[0].is_alive())
        self.assertEqual(self.drained, b"queued output\n")
        self.assert_child_stopped()

    def test_each_cleanup_failure_retains_primary_and_finishes_owned_cleanup(self):
        for failure, message in (("kill", "bootstrap kill"), ("wait", "bootstrap reap"),
                                 ("terminate", "job termination"), ("empty", "job emptiness"),
                                 ("join", "writer join"), ("close-input", "input close"),
                                 ("close-output", "output close"), ("close-job", "job close")):
            with self.subTest(failure=failure):
                # Each subcase has its own processes, barriers and pipe ownership.
                case = WindowsContainmentRefusal()
                try:
                    case.setUp()
                    with self.assertRaisesRegex(M.Refusal, "controlled source refusal") as caught:
                        case.run_bootstrap("spawn", errors=(failure,), input_bytes=b"x" * 22655)
                    self.assertIn("controlled " + message + " failure", str(caught.exception))
                    self.assertIn("controlled source refusal", str(caught.exception.__cause__))
                    self.assertIn("wait-empty", case.events)
                    self.assertIn("close-output", case.events)
                    self.assertIn("close-job", case.events)
                    case.assert_child_stopped()
                finally:
                    self.assertTrue(case.doCleanups(), "bootstrap fixture cleanup failed")

    def test_join_failure_never_closes_stdin_under_a_live_writer(self):
        with self.assertRaisesRegex(M.Refusal, "controlled source refusal") as caught:
            self.run_bootstrap("spawn", errors=("join",), input_bytes=b"x" * 22655,
                               live_writer=True)
        self.assertIn("writer join failure", str(caught.exception))
        self.assertIn("Git input shutdown deadline exceeded", str(caught.exception))
        self.assertTrue(self.writers[0].is_alive())
        self.assertNotIn("close-input", self.events)
        self.assertIn("close-output", self.events)
        self.assertIn("close-job", self.events)

    def test_combined_cleanup_failures_keep_every_failure_and_primary(self):
        errors = ("kill", "wait", "terminate", "empty", "join", "close-output", "close-job")
        with self.assertRaisesRegex(M.Refusal, "controlled source refusal") as caught:
            self.run_bootstrap("spawn", errors=errors, input_bytes=b"x" * 22655)
        for message in ("bootstrap kill", "bootstrap reap", "job termination", "job emptiness",
                        "writer join", "output close", "job close"):
            self.assertIn("controlled " + message + " failure", str(caught.exception))
        self.assertIn("controlled source refusal", str(caught.exception.__cause__))
        self.assert_child_stopped()

    def test_denied_bootstrap_kill_retains_the_actual_reap_timeout(self):
        with self.assertRaisesRegex(M.Refusal, "controlled source refusal") as caught:
            self.run_bootstrap("spawn", errors=("kill-denied",), input_bytes=b"x" * 22655)
        self.assertIn("controlled bootstrap kill denied", str(caught.exception))
        self.assertIn("TimeoutExpired", str(caught.exception))
        self.assertNotIn("bootstrap-exited", self.events)
        self.assertIn("wait-empty", self.events)
        self.assertIn("join-writer", self.events)
        self.assertIn("close-job", self.events)
        self.assert_child_stopped()

    def test_unobserved_bootstrap_exit_still_finishes_remaining_cleanup(self):
        with self.assertRaisesRegex(M.Refusal, "controlled source refusal") as caught:
            self.run_bootstrap("spawn", errors=("wait-unobserved",))
        self.assertIn("controlled bootstrap exit unobserved", str(caught.exception))
        self.assertNotIn("bootstrap-exited", self.events)
        self.assertIn("wait-empty", self.events)
        self.assertIn("close-output", self.events)
        self.assertIn("close-job", self.events)
        self.assert_child_stopped()

    def test_denied_job_termination_keeps_emptiness_and_pipe_failures(self):
        with self.assertRaisesRegex(M.Refusal, "controlled source refusal") as caught:
            self.run_bootstrap("spawn", errors=("terminate-denied",))
        self.assertIn("controlled job termination denied", str(caught.exception))
        self.assertIn("Git pipe shutdown deadline exceeded", str(caught.exception))
        self.assertIn("wait-empty", self.events)
        self.assertIn("close-output", self.events)
        self.assertIn("close-job", self.events)
        self.assert_child_stopped()

    def test_launch_and_job_cleanup_failures_are_all_retained(self):
        with self.assertRaisesRegex(M.Refusal, "controlled bootstrap launch failure") as caught:
            self.run_bootstrap("before", launch_failure=True,
                               errors=("terminate", "empty", "close-job"))
        for message in ("job termination", "job emptiness", "job close"):
            self.assertIn("controlled " + message + " failure", str(caught.exception))
        self.assertIn("controlled bootstrap launch failure", str(caught.exception.__cause__))
        self.assertEqual(self.events, ["terminate-job", "wait-empty", "close-job"])

    def test_launch_failure_still_terminates_and_verifies_the_empty_job(self):
        with self.assertRaisesRegex(OSError, "controlled bootstrap launch failure"):
            self.run_bootstrap("before", launch_failure=True)
        self.assertEqual(self.events, ["terminate-job", "wait-empty", "close-job"])
        self.assertEqual(self.owned_processes, [])

    def test_nonzero_exit_and_final_output_survive_job_close_failure(self):
        with self.assertRaisesRegex(M.Refusal, "Git command refused\nfinal failure") as caught:
            self.run_bootstrap("before", errors=("close-job",), nonzero=True)
        self.assertIn("controlled job close failure", str(caught.exception))
        self.assert_shutdown_order()

    def test_bootstrap_assignment_and_job_close_failures_are_both_retained(self):
        job = mock.Mock()
        job.enroll_bootstrap.side_effect = OSError("assignment refused")
        job.close.side_effect = OSError("bootstrap handle close refused")
        with mock.patch.object(M, "os", mock.Mock(name="nt", wraps=os)) as platform, \
                mock.patch.object(M, "WindowsJob", return_value=job), \
                mock.patch.object(M.subprocess, "call") as launch:
            platform.name = "nt"
            with self.assertRaisesRegex(M.Refusal, "assignment refused") as caught:
                M.contained_git("private-test-job", ["git", "--version"])
        self.assertIn("bootstrap handle close refused", str(caught.exception))
        self.assertIn("assignment refused", str(caught.exception.__cause__))
        job.close.assert_called_once_with()
        launch.assert_not_called()

    def test_job_initialization_and_handle_close_failures_are_both_retained(self):
        import ctypes
        api = mock.Mock()
        api.CreateJobObjectW.return_value = 1
        api.SetInformationJobObject.return_value = False
        api.CloseHandle.return_value = False
        with mock.patch.object(ctypes, "WinDLL", return_value=api, create=True), \
                mock.patch.object(ctypes, "get_last_error", return_value=0, create=True), \
                mock.patch.object(ctypes, "WinError", side_effect=[
                    OSError("job limits refused"), OSError("job handle close refused")], create=True):
            with self.assertRaisesRegex(M.Refusal, "job limits refused") as caught:
                M.WindowsJob("private-test-job")
        self.assertIn("job handle close refused", str(caught.exception))
        self.assertIn("job limits refused", str(caught.exception.__cause__))
        api.CloseHandle.assert_called_once_with(1)

    def test_bootstrap_assignment_failure_closes_job_before_any_command(self):
        job = mock.Mock()
        job.enroll_bootstrap.side_effect = OSError("assignment refused")
        with mock.patch.object(M.os, "name", "nt"), \
                mock.patch.object(M, "WindowsJob", return_value=job), \
                mock.patch.object(M.subprocess, "call", side_effect=AssertionError("uncontained launch")) as launch:
            with self.assertRaisesRegex(OSError, "assignment refused"):
                M.contained_git("private-test-job", ["git", "--version"])
        job.close.assert_called_once_with()
        launch.assert_not_called()


class OwnershipFieldDiagnostics(unittest.TestCase):
    def test_each_failed_field_is_named_without_disclosing_values(self):
        with tempfile.TemporaryDirectory(prefix="rmlui-fields-") as directory:
            w = M.Workspace(Path(directory).resolve(strict=True) / "build", specification())
            w.initialize()
            original = w.owner_path.read_bytes()
            changes = (("fields", {"extra": "secret"}), ("schema", {"schema": 2}),
                       ("root", {"root": "secret"}), ("nonce format", {"nonce": "secret"}),
                       ("nonce format", {"nonce": None}), ("delivery", {"delivery": "secret"}))
            for field, change in changes:
                with self.subTest(field=field):
                    value = json.loads(original)
                    value.update(change)
                    raw = M.encoded(value)
                    w.owner_path.write_bytes(raw)
                    with self.assertRaisesRegex(M.Refusal, field) as result:
                        w.owner()
                    self.assertNotIn("secret", str(result.exception))
                    self.assertEqual(w.owner_path.read_bytes(), raw)
            for field in w.spec.delivery:
                value = json.loads(original)
                value["delivery"][field] = "secret"
                raw = M.encoded(value)
                w.owner_path.write_bytes(raw)
                with self.assertRaisesRegex(M.Refusal, "delivery " + field) as result:
                    w.owner()
                self.assertNotIn("secret", str(result.exception))
                self.assertEqual(w.owner_path.read_bytes(), raw)
            w.owner_path.write_bytes(original)
            self.assertEqual(w.owner(), original)


class RootMismatchObservations(unittest.TestCase):
    def test_cli_root_refusal_reports_spellings_reader_and_nofollow_identity(self):
        with tempfile.TemporaryDirectory(prefix="rmlui-root-") as directory:
            root = Path(directory).resolve(strict=True)
            w = M.Workspace(root / "build", specification())
            w.initialize()
            other = root / "other"
            other.mkdir()
            for stored in (str(other), str(w.root) + os.sep, str(root / "missing")):
                with self.subTest(stored=stored):
                    value = json.loads(w.owner_path.read_bytes())
                    value["root"] = stored
                    raw = M.encoded(value)
                    w.owner_path.write_bytes(raw)
                    output = io.StringIO()
                    with contextlib.redirect_stderr(output):
                        self.assertEqual(M.main(["preflight", "--root", str(w.root)]), 1)
                    self.assertIn("workspace ownership root mismatch: ", output.getvalue())
                    details = json.loads(output.getvalue().split("workspace ownership root mismatch: ", 1)[1])
                    self.assertEqual(details["expected_root"], str(w.root))
                    self.assertEqual(details["stored_root"], stored)
                    self.assertEqual(details["interpreter"], sys.executable)
                    self.assertEqual(details["platform"], sys.platform)
                    self.assertEqual(details["os_name"], os.name)
                    info = os.lstat(w.root)
                    self.assertEqual(details["expected_identity"]["inode"], info.st_ino)
                    self.assertEqual(details["expected_identity"]["device"], info.st_dev)
                    self.assertEqual(w.owner_path.read_bytes(), raw)
                    self.assertEqual(set(p.name for p in w.root.iterdir()), {".rmlui-owner.json"})
                    self.assertNotIn(value["nonce"], output.getvalue())
                    self.assertNotIn("stored_identity", details)
                    if stored == str(w.root) + os.sep:
                        self.assertEqual(os.lstat(stored).st_ino, info.st_ino)

    def test_root_observations_preserve_refusal_when_metadata_is_unavailable(self):
        with tempfile.TemporaryDirectory(prefix="rmlui-root-") as directory:
            w = M.Workspace(Path(directory).resolve(strict=True) / "build", specification())
            w.initialize()
            for stored in (None, "", "private-field", "bad\0path", str(w.root.parent / "other")):
                with self.subTest(stored=stored):
                    value = json.loads(w.owner_path.read_bytes())
                    value["root"] = stored
                    raw = M.encoded(value)
                    w.owner_path.write_bytes(raw)
                    real_lstat = M.os.lstat
                    def lstat(path):
                        if path in (w.root, stored):
                            raise PermissionError(13, "private metadata detail")
                        return real_lstat(path)
                    with mock.patch.object(M.os, "lstat", side_effect=lstat):
                        with self.assertRaisesRegex(M.Refusal, "root mismatch: ") as caught:
                            w.owner()
                    details = json.loads(str(caught.exception).split("root mismatch: ", 1)[1])
                    self.assertEqual(details["expected_identity"], {"unavailable": "PermissionError", "errno": 13})
                    self.assertNotIn("stored_identity", details)
                    if not isinstance(stored, str) or not Path(stored).is_absolute():
                        self.assertEqual(details["stored_root"], "<invalid absolute root>")
                    self.assertNotIn("private", str(caught.exception))
                    self.assertEqual(w.owner_path.read_bytes(), raw)


class ConfigureCallerBoundaries(unittest.TestCase):
    sites = (("run-coverage.ps1", "coverage", "$buildDir", "coverage"),
             ("validate-engine-frontier.ps1", "debug-asan", "$asanBuildDir", "debug-asan"),
             ("validate-engine-frontier.ps1", "debug-simo0", "$parityDir", "$parityPreset"))

    def test_caller_changes_select_the_registered_source_contract_suite(self):
        loader = importlib.util.spec_from_file_location("ci_paths", ROOT / "tools/ci/ci_paths.py")
        paths = importlib.util.module_from_spec(loader)
        loader.loader.exec_module(paths)
        for name in ("CMakePresets.json", "tools/gates/run-coverage.ps1",
                     "tools/gates/validate-engine-frontier.ps1", "README.md", "CONTRIBUTING.md",
                     "docs/development.md", "docs/static-morph-assets.md", "docs/performance.md",
                     "docs/index.md", "docs/known-limitations.md", "docs/static-preview.md",
                     "docs/dependency-source.md"):
            with self.subTest(path=name):
                self.assertTrue(paths.needs_python(name))
        self.assertIn("tools/ci/test_rmlui_source.py", paths.PYTHON_TESTS)

    def test_prefab_recipe_enrolls_a_fresh_root_and_binds_all_consumers(self):
        loader = importlib.util.spec_from_file_location("recipe_paths", ROOT / "tools/ci/ci_paths.py")
        paths = importlib.util.module_from_spec(loader)
        loader.loader.exec_module(paths)
        name = "tools/prefab_runtime/README.md"
        self.assertTrue(paths.needs_python(name), name)
        self.assertIn("tools/ci/test_rmlui_source.py", paths.PYTHON_TESTS)
        source = (ROOT / name).read_text(encoding="utf-8", errors="strict")
        recipe = re.search(r"```sh\n(.*?)\n```", source, re.DOTALL)
        self.assertIsNotNone(recipe)
        self.assertEqual(recipe[1].splitlines(), [
            "python3 -I -B tools/ci/rmlui_source.py init --root build/release-prefab || exit",
            "python3 -I -B tools/ci/rmlui_source.py preflight --root build/release-prefab || exit",
            'cmake --preset release -B build/release-prefab '
            '"-DPython3_EXECUTABLE=$(python3 -I -B -c \'import sys; print(sys.executable)\')" '
            '-DLUMINUMBRA_ENABLE_PREFAB_RUNTIME=ON',
            "cmake --build build/release-prefab --target luminumbra_prefab_inspect prefab_runtime_test --parallel 2",
            "ctest --test-dir build/release-prefab -R '^Prefab(Digest|RuntimeTest)\\.' "
            "--no-tests=error --output-on-failure",
            "cmake --install build/release-prefab --prefix /your/install --component PrefabRuntime",
        ])
        presets = json.loads((ROOT / "CMakePresets.json").read_bytes())["configurePresets"]
        self.assertIn("release", {preset["name"] for preset in presets})

    def test_manual_preset_examples_enroll_only_absent_matching_roots(self):
        paths = ("README.md", "CONTRIBUTING.md", "docs/development.md", "docs/static-morph-assets.md",
                 "docs/performance.md", "docs/index.md", "docs/known-limitations.md", "docs/static-preview.md")
        for name in paths:
            source = (ROOT / name).read_text(encoding="utf-8")
            for configure in re.finditer(r"(?m)^( *)cmake --preset ([a-z-]+)", source):
                root = "build/" + configure[2]
                before = source[:configure.start()]
                self.assertTrue(before.endswith(f"preflight --root {root} || exit\n"))
                self.assertIn(f"if [ ! -e {root} ]; then", before)
                self.assertIn(f"python3 -I -B tools/ci/rmlui_source.py init --root {root} || exit", before)

    def test_preset_roots_and_absent_only_initializer_before_every_gate_configure(self):
        presets = {p["name"]: p for p in json.loads((ROOT / "CMakePresets.json").read_bytes())["configurePresets"]}
        for file, preset, variable, argument in self.sites:
            with self.subTest(preset=preset):
                node = presets[preset]
                for _ in range(len(presets)):
                    if "binaryDir" in node:
                        break
                    node = presets[node["inherits"]]
                self.assertEqual(node["binaryDir"], "${sourceDir}/build/${presetName}")
                source = (ROOT / "tools/gates" / file).read_text(encoding="utf-8")
                call = "& cmake --preset " + argument
                self.assertEqual(source.count(call), 1)
                before = source.split(call)[0]
                self.assertTrue("        # Enroll only" in before, "configure caller lacks enrollment guard")
                fragment = before[before.rindex("        # Enroll only"):].strip()
                expected = (
                    f"if (-not (Test-Path -LiteralPath {variable})) {{",
                    f"& python -I -B tools/ci/rmlui_source.py init --root {variable}",
                    'if ($LASTEXITCODE -ne 0) { throw "RmlUi workspace initialization failed ($LASTEXITCODE)" }',
                    f"& python -I -B tools/ci/rmlui_source.py preflight --root {variable}",
                    'if ($LASTEXITCODE -ne 0) { throw "RmlUi workspace preflight failed ($LASTEXITCODE)" }',
                )
                for line in expected:
                    self.assertIn(line, fragment)
                self.assertLess(fragment.index(expected[0]), fragment.index(expected[1]))
                self.assertLess(fragment.index(expected[1]), fragment.index(expected[2]))
                self.assertLess(fragment.index(expected[2]), fragment.index(expected[3]))
                self.assertLess(fragment.index(expected[3]), fragment.index(expected[4]))
                with tempfile.TemporaryDirectory(prefix="rmlui-configure-") as directory:
                    root = Path(directory).resolve(strict=True) / "build" / preset
                    # Boundary model follows the verified guard; only the real
                    # helper runs. No CMake or native gate commands execute.
                    def boundary():
                        if not os.path.lexists(root):
                            init = subprocess.run([sys.executable, "-I", "-B", str(MODULE_PATH),
                                                   "init", "--root", str(root)], capture_output=True, check=False)
                            if init.returncode:
                                return init
                        return subprocess.run([sys.executable, "-I", "-B", str(MODULE_PATH),
                                               "preflight", "--root", str(root)], capture_output=True, check=False)
                    result = boundary()
                    self.assertEqual(result.returncode, 0, result.stderr)
                    owner = (root / ".rmlui-owner.json").read_bytes()
                    self.assertEqual(boundary().returncode, 0)
                    self.assertEqual((root / ".rmlui-owner.json").read_bytes(), owner)
                    (root / ".rmlui-owner.json").unlink()
                    (root / "foreign").write_bytes(b"foreign")
                    self.assertNotEqual(boundary().returncode, 0)
                    self.assertEqual((root / "foreign").read_bytes(), b"foreign")
                    self.assertFalse((root / ".rmlui-owner.json").exists())
                    (root / "foreign").unlink()
                    root.rmdir()
                    root.parent.rmdir()
                    root.parent.write_bytes(b"occupied parent")
                    self.assertNotEqual(boundary().returncode, 0)
                    self.assertEqual(root.parent.read_bytes(), b"occupied parent")


class Utf8SourceReads(unittest.TestCase):
    def test_existing_workflow_and_registration_cases_ignore_cp1252_default(self):
        read = Path.read_text
        def cp1252(path, *args, **kwargs):
            kwargs.setdefault("encoding", "cp1252")
            kwargs.setdefault("errors", "strict")
            return read(path, *args, **kwargs)
        cases = [DeliveryRefusals("test_cmake_and_ci_registration_preserve_population_and_verification_order"),
                 PairedWorkflowInitialization("test_distinct_revisions_initialize_with_their_own_helpers")]
        with mock.patch.object(Path, "read_text", cp1252):
            for case in cases:
                result = unittest.TestResult()
                case.run(result)
                self.assertTrue(result.wasSuccessful(), str(result.errors) + str(result.failures))


class FixtureCleanupCustody(unittest.TestCase):
    def test_readonly_object_retry_requires_exact_custody_attributes_and_unlink(self):
        for name, attributes, winerror, allowed in (
                ("objects/pack/pack-" + "a" * 40 + ".idx", 1, 5, True),
                ("objects/pack/pack-" + "a" * 40 + ".idx", 0, 5, False),
                ("objects/pack/pack-" + "a" * 40 + ".idx", M.REPARSE | 1, 5, False),
                ("objects/pack/pack-" + "a" * 40 + ".idx", 1, 32, False),
                ("config", 1, 5, False)):
            with self.subTest(name=name, attributes=attributes, winerror=winerror):
                with tempfile.TemporaryDirectory(prefix="rmlui-cleanup-") as directory:
                    root = Path(directory).resolve(strict=True)
                    source = root / "build" / "source"
                    path = source / ".git" / name
                    path.parent.mkdir(parents=True)
                    path.write_bytes(b"owned object")
                    identity = file_identity(root)
                    real_stat = os.lstat
                    class Info:
                        def __init__(self, original):
                            self.original = original
                            self.st_file_attributes = attributes
                        def __getattr__(self, field):
                            return getattr(self.original, field)
                    def lstat(candidate, *args, **kwargs):
                        info = real_stat(candidate, *args, **kwargs)
                        return Info(info) if Path(candidate) == path else info
                    error = PermissionError("controlled Windows removal refusal")
                    error.winerror = winerror
                    def rmtree(candidate, onerror=None):
                        self.assertEqual(candidate, root)
                        if onerror is None:
                            raise error
                        onerror(os.unlink, str(path), (PermissionError, error, None))
                    # Inventory must also reject reparse entries before deletion.
                    with mock.patch.object(os, "lstat", side_effect=lstat),                             mock.patch.object(shutil, "rmtree", side_effect=rmtree),                             mock.patch(__name__ + ".clear_fixture_readonly") as clear:
                        if allowed:
                            remove_source_fixture(root, identity, (source,))
                            clear.assert_called_once_with(path, mock.ANY)
                            self.assertFalse(path.exists())
                        else:
                            with self.assertRaises((PermissionError, M.Refusal)):
                                remove_source_fixture(root, identity, (source,))
                            clear.assert_not_called()
                            self.assertEqual(path.read_bytes(), b"owned object")
                    with self.assertRaisesRegex(AssertionError, "custody"):
                        remove_source_fixture(root, (-1, -1), (source,))

    def test_windows_readonly_pack_removal_and_live_handle_refusal(self):
        # The portable model above always runs. Windows additionally exercises
        # the actual attribute and exclusive-handle APIs on its own allocation.
        if os.name == "nt":
            with tempfile.TemporaryDirectory(prefix="rmlui-readonly-") as directory:
                root = Path(directory).resolve(strict=True) / "fixture"
                source = root / "source"
                path = source / ".git/objects/pack" / ("pack-" + "a" * 40 + ".idx")
                path.parent.mkdir(parents=True)
                path.write_bytes(b"owned readonly pack index")
                path.chmod(stat.S_IREAD)
                identity = file_identity(root)
                with path.open("rb") as live:
                    self.assertEqual(live.read(5), b"owned")
                    with self.assertRaises(PermissionError):
                        remove_source_fixture(root, identity, (source,))
                    self.assertTrue(os.lstat(path).st_file_attributes & stat.FILE_ATTRIBUTE_READONLY)
                remove_source_fixture(root, identity, (source,))
                self.assertFalse(root.exists())
        else:
            self.test_readonly_object_retry_requires_exact_custody_attributes_and_unlink()


if __name__ == "__main__":
    unittest.main()
