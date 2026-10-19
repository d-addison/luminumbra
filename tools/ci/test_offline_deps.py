#!/usr/bin/env python3
"""Isolated stdlib contracts for offline dependency provisioning."""

import errno
import ctypes
from contextlib import contextmanager, redirect_stderr
import importlib.util
import io
import json
import os
from pathlib import Path
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from types import SimpleNamespace
from unittest.mock import MagicMock, patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("offline_deps", ROOT / "tools/ci/offline_deps.py")
deps = importlib.util.module_from_spec(SPEC)
if __name__ == "__main__":
    sys.stderr.write("Offline dependencies: LOAD START\n")
    sys.stderr.flush()
SPEC.loader.exec_module(deps)
if __name__ == "__main__":
    sys.stderr.write(
        f"Offline dependencies: LOAD FINISH monotonic_ns={time.monotonic_ns()} "
        "phase=load_finish ordinal=0\n"
    )
    sys.stderr.flush()


class GitConfigScannerTests(unittest.TestCase):
    def test_include_sections_cover_ascii_whitespace_and_quoted_conditions(self):
        for whitespace in (" ", "\t", "\r", "\v", "\f"):
            for section in ("include", 'includeIf "gitdir:never-matches"',
                            'INCLUDEIF "onbranch:a]b"', 'includeIf "gitdir:a\\"b"',
                            "include.path"):
                for text in (f'{whitespace}[{section}]\npath = outside\n',
                             f'[core]{whitespace}[{section}]\r\npath = outside\r\n',
                             f'[{section}{whitespace}]\npath = outside\n'):
                    with self.subTest(text=text):
                        self.assertTrue(deps.config_has_includes(text))
            self.assertTrue(deps.config_has_includes(
                f'[includeIf{whitespace}"gitdir:*"]\npath = outside\n'))

    def test_values_comments_and_continuations_do_not_create_sections(self):
        for text in ('[fixture]\ntext = "[include]"\n',
                     '[fixture]\ntext = "a\\\n[include]"\n',
                     '[fixture]\ntext = a\\\n[include]\n',
                     '[fixture "a]b"]\ntext = "escaped \\" [include]"\n',
                     '[fixture]\ntext = "#;[includeIf]" ; [include]\n',
                     '# [include]\n; [includeIf "gitdir:*"]\n', ""):
            with self.subTest(text=text):
                self.assertFalse(deps.config_has_includes(text))
        self.assertTrue(deps.config_has_includes(
            '[fixture]\ntext = "a\\\n[include]"\n\f[include]\npath = outside\n'))

    def test_partial_and_unsupported_sections_fail_closed(self):
        for text in ('[', '[include', '[includeIf "unterminated]',
                     '[includeIf\n"gitdir:*"]', '[core] [include\\\n]',
                     '[core] [include\x00]', '[core] [include\u00a0]'):
            with self.subTest(text=text):
                with self.assertRaisesRegex(deps.DependencyError, "unsupported Git config section"):
                    deps.config_has_includes(text)


class OfflineDependenciesTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="offline deps ")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name).resolve()
        self.root = self.base / "clone with spaces"
        (self.root / "vendor").mkdir(parents=True)
        self.vendor = self.root / "vendor/CMakeLists.txt"
        self.vendor.write_text('FetchContent_Declare(sample GIT_REPOSITORY https://example.org/sample.git GIT_TAG v1 GIT_SHALLOW TRUE)\n')
        self.cache = self.base / "shared cache"
        self.source = self.cache / "sample"
        self.source.mkdir(parents=True)
        (self.source / ".git").mkdir()
        (self.source / "input with spaces.h").write_text("source content\n")
        self.commit = "a" * 40
        self.lock = self.root / "lock.json"
        self.write_lock()
        self.git_mock = patch.object(deps, "git", side_effect=self.fake_git).start()
        self.addCleanup(patch.stopall)

    def fake_git(self, source, *args, **kwargs):
        if args == ("rev-parse", "--verify", "HEAD^0"):
            return self.commit
        if args == ("ls-tree", "-rz", "HEAD"):
            return ""
        self.fail(f"unexpected Git call: {args}")

    def write_lock(self):
        self.data = deps.unresolved_lock(self.root)
        self.data["status"] = "resolved"
        self.data["dependencies"]["sample"].update(
            commit=self.commit, sha256=deps.source_digest(self.source), submodules={})
        self.save_lock()

    def save_lock(self):
        self.lock.write_text(json.dumps(self.data))

    def verify(self):
        return deps.verify(self.root, self.cache, self.lock)

    def test_windows_reparse_sources_metadata_and_stage_components_are_refused(self):
        nested = self.source / "nested"
        nested.mkdir()
        (nested / "same.h").write_text("same bytes\n")
        scratch = self.root / "scratch"
        scratch.mkdir()
        self.write_lock()
        real_lstat = os.lstat
        marked = None

        def metadata(path, *args, **kwargs):
            info = real_lstat(path, *args, **kwargs)
            fields = {name: getattr(info, name) for name in dir(info) if name.startswith("st_")}
            # Junctions and unknown reparse tags can retain directory mode.
            fields["st_file_attributes"] = 0x400 if Path(path) == marked else 0
            return SimpleNamespace(**fields)

        with patch.object(deps._PATH_SAFETY, "WINDOWS", True), \
                patch.object(os, "lstat", side_effect=metadata):
            for marked in (self.source, nested, nested / "same.h", self.source / ".git", scratch):
                with self.subTest(marked=marked):
                    with self.assertRaisesRegex(deps.DependencyError, "reparse point"):
                        self.stage()
                    self.assertFalse((scratch / "copied sources").exists())
                    self.assertFalse((scratch / "initial cache.cmake").exists())
            marked = self.source / ".git"
            with self.assertRaisesRegex(deps.DependencyError, "reparse point"):
                deps.git_metadata(self.source, self.cache)

    def test_windows_reparse_injected_after_verification_is_refused_during_copy(self):
        nested = self.source / "nested"
        nested.mkdir()
        (nested / "same.h").write_text("same bytes\n")
        self.write_lock()
        real_lstat, real_verify = os.lstat, deps.verify
        inject = False

        def verified(*args):
            nonlocal inject
            result = real_verify(*args)
            inject = True
            return result

        def metadata(path, *args, **kwargs):
            info = real_lstat(path, *args, **kwargs)
            fields = {name: getattr(info, name) for name in dir(info) if name.startswith("st_")}
            fields["st_file_attributes"] = 0x400 if inject and Path(path) == nested else 0
            return SimpleNamespace(**fields)

        with patch.object(deps._PATH_SAFETY, "WINDOWS", True), \
                patch.object(os, "lstat", side_effect=metadata), \
                patch.object(deps, "verify", side_effect=verified):
            with self.assertRaisesRegex(deps.DependencyError, "reparse point"):
                self.stage()
        self.assertFalse((self.root / "scratch/copied sources").exists())
        self.assertFalse((self.root / "scratch/initial cache.cmake").exists())
        self.assertEqual((nested / "same.h").read_text(), "same bytes\n")

    def test_real_windows_junction_to_identical_content_is_refused(self):
        if os.name != "nt":
            self.skipTest("directory junction creation requires Windows")
        nested = self.source / "nested"
        nested.mkdir()
        (nested / "same.h").write_text("same bytes\n")
        self.write_lock()
        outside = self.base / "outside"
        nested.rename(outside)
        try:
            created = subprocess.run(["cmd", "/d", "/c", "mklink", "/J", str(nested), str(outside)],
                                     capture_output=True, check=False)
        except OSError as error:
            self.skipTest(f"directory junction creation unavailable: {error}")
        if created.returncode:
            self.skipTest(f"directory junction creation unavailable: {created.stderr!r}")
        try:
            for action in (lambda: deps.source_digest(self.source), self.verify, self.stage):
                with self.subTest(action=action), self.assertRaisesRegex(deps.DependencyError, "reparse point"):
                    action()
            self.assertEqual((outside / "same.h").read_text(), "same bytes\n")
            self.assertFalse((self.root / "scratch/copied sources").exists())
        finally:
            os.rmdir(nested)

    def test_windows_reparse_introduced_at_file_copy_is_refused(self):
        real_copytree = deps.shutil.copytree
        source = self.source / "input with spaces.h"
        inject = False

        def copytree(*args, **kwargs):
            copy_file = kwargs.get("copy_function", shutil.copy2)

            def changed(path, destination):
                nonlocal inject
                inject = True
                return copy_file(path, destination)

            kwargs["copy_function"] = changed
            return real_copytree(*args, **kwargs)

        with patch.object(deps._PATH_SAFETY, "WINDOWS", True), \
                patch.object(deps._PATH_SAFETY, "is_reparse_point", side_effect=lambda p:
                             inject and Path(p) == source), \
                patch.object(deps.shutil, "copytree", side_effect=copytree):
            with self.assertRaisesRegex(deps.DependencyError, "reparse point"):
                self.stage()
        self.assertFalse((self.root / "scratch/copied sources").exists())
        self.assertFalse((self.root / "scratch/initial cache.cmake").exists())
        self.assertEqual(source.read_text(), "source content\n")

    def test_windows_reparse_cleanup_root_is_not_traversed(self):
        destination = self.base / "cleanup root"
        destination.mkdir()
        content = destination / "preserved"
        content.write_text("preserved\n")
        with patch.object(deps._PATH_SAFETY, "WINDOWS", True), \
                patch.object(deps._PATH_SAFETY, "is_reparse_point", side_effect=lambda p:
                             Path(p) == destination):
            with self.assertRaisesRegex(deps.DependencyError, "reparse point"):
                deps.remove_stage(destination)
        self.assertEqual(content.read_text(), "preserved\n")

    def stage(self, dest=None, init=None):
        deps.stage(self.root, self.cache, self.lock,
                   dest or self.root / "scratch/copied sources",
                   init or self.root / "scratch/initial cache.cmake")

    def test_declared_lock_matches_all_eighteen_dependencies(self):
        declared = deps.declarations(ROOT)
        self.assertEqual(set(declared), set("entt glm glfw glad nlohmann_json joltphysics freetype rmlui soil2 miniaudio meshoptimizer lua_upstream sol2 spdlog imgui_src stb googletest lz4_upstream".split()))
        committed = json.loads((ROOT / "tools/ci/deps-lock.json").read_text())
        for name, expected in declared.items():
            for key, value in expected.items():
                self.assertEqual(committed["dependencies"][name][key], value)

    def test_unresolved_lock_refuses_verify_and_stage_without_writes(self):
        self.lock.write_text(json.dumps(deps.unresolved_lock(self.root)))
        for action in (self.verify, self.stage):
            with self.assertRaisesRegex(deps.DependencyError, "unresolved"):
                action()
        self.assertFalse((self.root / "scratch/copied sources").exists())

    def test_missing_and_extra_lock_dependencies_refused(self):
        for names in ({}, {**self.data["dependencies"], "extra": {}}):
            self.data["dependencies"] = names
            self.save_lock()
            with self.assertRaisesRegex(deps.DependencyError, "dependency set"):
                self.verify()

    def test_wrong_declared_revision_or_repository_refused(self):
        for key in ("revision", "repository"):
            self.write_lock()
            self.data["dependencies"]["sample"][key] = "wrong"
            self.save_lock()
            with self.assertRaisesRegex(deps.DependencyError, "wrong declared"):
                self.verify()

    def test_wrong_commit_and_unresolved_digest_refused(self):
        self.data["dependencies"]["sample"]["commit"] = "b" * 40
        self.save_lock()
        with self.assertRaisesRegex(deps.DependencyError, "wrong resolved"):
            self.verify()
        self.data["dependencies"]["sample"]["sha256"] = None
        self.save_lock()
        with self.assertRaisesRegex(deps.DependencyError, "unresolved"):
            self.verify()

    def test_malformed_lock_schema_refused(self):
        for malformed in ([], {"version": 1, "status": "resolved", "dependencies": []},
                          {"version": 1, "status": "resolved", "dependencies": {"sample": "invalid"}}):
            self.lock.write_text(json.dumps(malformed))
            with self.assertRaises(deps.DependencyError):
                self.verify()

    def test_fixed_revision_must_equal_resolved_commit(self):
        self.vendor.write_text(f'FetchContent_Declare(sample GIT_REPOSITORY https://example.org/sample.git GIT_TAG {"b" * 40})')
        self.write_lock()
        with self.assertRaisesRegex(deps.DependencyError, "pinned revision"):
            self.verify()

    def test_missing_source_refused(self):
        (self.source / "input with spaces.h").unlink()
        (self.source / ".git").rmdir()
        self.source.rmdir()
        with self.assertRaisesRegex(deps.DependencyError, "missing source"):
            self.verify()

    def test_digest_changes_for_content_path_and_extra_file(self):
        original = deps.source_digest(self.source)
        source = self.source / "input with spaces.h"
        source.write_text("changed\n")
        with self.assertRaisesRegex(deps.DependencyError, "digest mismatch"):
            self.verify()
        source.write_text("source content\n")
        self.assertEqual(original, deps.source_digest(self.source))
        source.rename(self.source / "renamed.h")
        self.assertNotEqual(original, deps.source_digest(self.source))
        (self.source / "renamed.h").rename(source)
        (self.source / "extra").write_text("extra")
        self.assertNotEqual(original, deps.source_digest(self.source))

    @unittest.skipUnless(os.name == "posix", "Windows chmod does not implement POSIX executable bits")
    def test_digest_changes_for_posix_executable_bits(self):
        original = deps.source_digest(self.source)
        source = self.source / "input with spaces.h"
        mode = source.stat().st_mode
        self.addCleanup(source.chmod, mode)
        source.chmod(mode | 0o111)
        self.assertNotEqual(original, deps.source_digest(self.source))
        source.chmod(mode)
        self.assertEqual(original, deps.source_digest(self.source))

    def test_readonly_attribute_does_not_change_content_digest(self):
        # Windows chmod supports the readonly attribute; it is not executable
        # identity. Exercise that real permission change on every platform.
        original = deps.source_digest(self.source)
        source = self.source / "input with spaces.h"
        mode = source.stat().st_mode
        try:
            source.chmod(mode & ~0o222)
            self.assertEqual(original, deps.source_digest(self.source))
            self.verify()
        finally:
            source.chmod(mode)

    def test_git_metadata_is_not_part_of_source_digest(self):
        (self.source / ".git/metadata").write_text("mutable Git bookkeeping")
        self.verify()

    def test_stage_copies_without_hardlinks_and_writes_all_overrides(self):
        self.stage()
        dest = self.root / "scratch/copied sources"
        copied = dest / "sample/input with spaces.h"
        self.assertEqual(copied.read_text(), "source content\n")
        self.assertNotEqual(copied.stat().st_ino, (self.source / copied.name).stat().st_ino)
        copied.write_text("independent copy")
        self.assertEqual((self.source / copied.name).read_text(), "source content\n")
        self.assertFalse((dest / "sample/.git").exists())
        text = (self.root / "scratch/initial cache.cmake").read_text()
        self.assertIn("FETCHCONTENT_SOURCE_DIR_SAMPLE", text)
        self.assertIn("FETCHCONTENT_BASE_DIR", text)
        self.assertIn("FETCHCONTENT_FULLY_DISCONNECTED ON", text)
        self.assertIn("GLAD_REPRODUCIBLE ON", text)
        self.assertIn(str(dest / "sample"), text)

    def test_stage_all_real_declaration_overrides(self):
        (self.root / "vendor/CMakeLists.txt").write_text((ROOT / "vendor/CMakeLists.txt").read_text())
        lock = deps.unresolved_lock(self.root)
        lock["status"] = "resolved"
        for name in lock["dependencies"]:
            source = self.cache / name
            source.mkdir(exist_ok=True)
            (source / "source.h").write_text(name)
            lock["dependencies"][name]["sha256"] = deps.source_digest(source)
        with patch.object(deps, "verify", return_value=lock):
            self.stage()
        text = (self.root / "scratch/initial cache.cmake").read_text()
        self.assertEqual(text.count("set(FETCHCONTENT_SOURCE_DIR_"), 18)
        for name in lock["dependencies"]:
            self.assertIn(f"FETCHCONTENT_SOURCE_DIR_{name.upper()} ", text)

    def test_path_traversal_and_symlink_escape_refused(self):
        outside = self.base / "outside"
        outside.mkdir()
        for dest, init in ((self.root / "scratch/../../escape", self.root / "scratch/init"),
                           (self.root / "scratch/dest", outside / "init")):
            with self.assertRaises(deps.DependencyError):
                self.stage(dest, init)
        if os.name == "nt":
            return
        (self.root / "scratch").mkdir(exist_ok=True)
        (self.root / "scratch/link").symlink_to(outside, target_is_directory=True)
        with self.assertRaises(deps.DependencyError):
            self.stage(self.root / "scratch/link/dest")
        (self.source / "escape").symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(deps.DependencyError, "symlink escapes"):
            deps.source_digest(self.source)

    @unittest.skipIf(os.name == "nt", "POSIX source symlinks")
    def test_safe_relative_symlink_copied_and_absolute_link_rejected(self):
        (self.source / "alias").symlink_to("input with spaces.h")
        self.write_lock()
        self.stage()
        copied = self.root / "scratch/copied sources/sample/alias"
        self.assertTrue(copied.is_symlink())
        self.assertEqual(copied.read_text(), "source content\n")
        copied.unlink()
        (self.source / "alias").unlink()
        (self.source / "alias").symlink_to(self.source / "input with spaces.h")
        with self.assertRaisesRegex(deps.DependencyError, "symlink escapes"):
            deps.source_digest(self.source)

    def test_existing_stage_paths_refused(self):
        self.stage()
        with self.assertRaisesRegex(deps.DependencyError, "fresh"):
            self.stage()

    def fixture_state(self):
        return {p.relative_to(self.base).as_posix():
                ("link", os.readlink(p)) if p.is_symlink() else
                ("directory",) if p.is_dir() else ("file", p.read_bytes())
                for p in self.base.rglob("*")}

    def assert_symlinked_stage_refused_without_writes(self, dest=None, init=None):
        before = self.fixture_state()
        with patch.object(deps, "verify", side_effect=AssertionError("verification ran")):
            with self.assertRaisesRegex(deps.DependencyError, "symlinked"):
                self.stage(dest, init)
        self.assertEqual(self.fixture_state(), before)

    def test_symlinked_scratch_root_refused_without_writes(self):
        docs = self.root / "docs"
        docs.mkdir()
        (docs / "keep.txt").write_text("existing documentation")
        try:
            (self.root / "scratch").symlink_to(docs, target_is_directory=True)
        except (NotImplementedError, OSError) as error:
            self.skipTest(f"symlinks unavailable: {error}")
        self.assert_symlinked_stage_refused_without_writes()

    def test_symlinked_scratch_diagnostic_precedes_reparse_validation(self):
        scratch = self.root / "scratch"
        is_symlink = Path.is_symlink
        before = self.fixture_state()
        # No symlink privilege is needed to check refusal ordering on Windows.
        with patch.object(Path, "is_symlink", lambda path: path == scratch or is_symlink(path)), \
                patch.object(deps, "reject_reparse_points", side_effect=AssertionError("reparse lookup ran")), \
                patch.object(deps, "verify", side_effect=AssertionError("verification ran")):
            with self.assertRaisesRegex(deps.DependencyError, "symlinked scratch root"):
                self.stage()
        self.assertEqual(self.fixture_state(), before)

    def test_symlinked_intermediate_stage_components_refused_without_writes(self):
        scratch = self.root / "scratch"
        target = scratch / "existing target"
        target.mkdir(parents=True)
        (target / "keep.txt").write_text("existing content")
        nested = scratch / "nested"
        nested.mkdir()
        for link in (nested / "link", self.root / "alias"):
            try:
                link.symlink_to(target, target_is_directory=True)
            except (NotImplementedError, OSError) as error:
                self.skipTest(f"symlinks unavailable: {error}")
            for suffix in (Path("new parent/output"), Path("../new parent/output")):
                for output in ("dest", "init"):
                    with self.subTest(link=link.name, suffix=str(suffix), output=output):
                        self.assert_symlinked_stage_refused_without_writes(
                            **{output: link / suffix})

    def test_dangling_stage_path_symlinks_refused_without_writes(self):
        scratch = self.root / "scratch"
        scratch.mkdir()
        link = scratch / "dangling"
        try:
            link.symlink_to("missing", target_is_directory=True)
        except (NotImplementedError, OSError) as error:
            self.skipTest(f"symlinks unavailable: {error}")
        for output in ("dest", "init"):
            with self.subTest(output=output):
                self.assert_symlinked_stage_refused_without_writes(**{output: link})

    def test_ordinary_nested_stage_destinations_accepted(self):
        dest = self.root / "scratch/nested sources/deeper/copied sources"
        init = self.root / "scratch/nested cache/deeper/initial cache.cmake"
        before_lock = self.lock.read_bytes()
        before_digest = deps.source_digest(self.source)
        self.stage(dest, init)
        self.assertEqual((dest / "sample/input with spaces.h").read_text(), "source content\n")
        self.assertIn(str(dest / "sample"), init.read_text())
        self.assertIn("FETCHCONTENT_FULLY_DISCONNECTED ON", init.read_text())
        self.assertEqual(self.lock.read_bytes(), before_lock)
        self.assertEqual(deps.source_digest(self.source), before_digest)

    def test_existing_initial_cache_is_preserved_without_creating_stage(self):
        init = self.root / "scratch/initial cache.cmake"
        init.parent.mkdir()
        init.write_text("existing cache")
        with self.assertRaisesRegex(deps.DependencyError, "fresh"):
            self.stage()
        self.assertEqual(init.read_text(), "existing cache")
        self.assertFalse((self.root / "scratch/copied sources").exists())

    def test_partial_initial_cache_write_is_removed_and_retry_succeeds(self):
        init = self.root / "scratch/initial cache.cmake"
        original_lock = self.lock.read_bytes()
        original_digest = deps.source_digest(self.source)
        real_temporary = deps.tempfile.NamedTemporaryFile
        def temporary(*args, **kwargs):
            handle = real_temporary(*args, **kwargs)
            self.assertEqual(Path(handle.name).parent, init.parent)
            write = handle.write
            def fail(value):
                write(value[:20])
                handle.flush()
                self.assertFalse(init.exists())
                raise OSError("injected partial cache write")
            handle.write = fail
            return handle
        with patch.object(deps.tempfile, "NamedTemporaryFile", side_effect=temporary):
            with self.assertRaisesRegex(OSError, "partial cache write"):
                self.stage()
        self.assertFalse(init.exists())
        self.assertFalse((self.root / "scratch/copied sources").exists())
        self.assertEqual(list(init.parent.glob(init.name + ".*.tmp")), [])
        self.assertEqual(self.lock.read_bytes(), original_lock)
        self.assertEqual(deps.source_digest(self.source), original_digest)
        self.stage()
        self.assertIn("FETCHCONTENT_SOURCE_DIR_SAMPLE", init.read_text())

    def test_initial_cache_replace_failure_cleans_up_and_retry_succeeds(self):
        init = self.root / "scratch/initial cache.cmake"
        real_replace = deps.os.link
        def fail(source, target):
            self.assertEqual(Path(source).parent, Path(target).parent)
            self.assertFalse(Path(target).exists())
            self.assertIn("FETCHCONTENT_SOURCE_DIR_SAMPLE", Path(source).read_text())
            raise OSError("injected cache publication failure")
        with patch.object(deps.os, "link", side_effect=fail):
            with self.assertRaisesRegex(OSError, "publication failure"):
                self.stage()
        self.assertFalse(init.exists())
        self.assertFalse((self.root / "scratch/copied sources").exists())
        self.assertEqual(list(init.parent.glob(init.name + ".*.tmp")), [])
        with patch.object(deps.os, "link", wraps=real_replace) as replace:
            self.stage()
        self.assertEqual(replace.call_count, 1)
        self.assertIn("FETCHCONTENT_FULLY_DISCONNECTED ON", init.read_text())

    def readonly_source(self):
        directory = self.source / "readonly directory"
        directory.mkdir()
        content = directory / "readonly.h"
        content.write_text("readonly content\n")
        for path in (directory, content):
            mode = path.stat().st_mode
            self.addCleanup(path.chmod, mode)
            path.chmod(mode & ~0o222)
        self.write_lock()
        return directory, content

    def test_readonly_stage_publication_failure_cleans_up_and_retry_succeeds(self):
        directory, content = self.readonly_source()
        modes = [path.stat().st_mode for path in (directory, content)]
        dest = self.root / "scratch/copied sources"
        init = self.root / "scratch/initial cache.cmake"
        original = OSError("injected cache publication failure")

        def fail(source, target):
            self.assertEqual(Path(target), init)
            self.assertEqual((dest / "sample/readonly directory/readonly.h").read_text(),
                             "readonly content\n")
            self.assertFalse((dest / "sample/readonly directory").stat().st_mode & stat.S_IWUSR)
            raise original

        with patch.object(deps.os, "link", side_effect=fail):
            with self.assertRaises(OSError) as caught:
                self.stage()
        self.assertIs(caught.exception, original)
        self.assertFalse(dest.exists())
        self.assertFalse(init.exists())
        self.assertEqual(list(init.parent.glob(init.name + ".*.tmp")), [])
        self.assertEqual([path.stat().st_mode for path in (directory, content)], modes)
        self.stage()
        self.assertEqual((dest / "sample/readonly directory/readonly.h").read_text(),
                         "readonly content\n")
        self.assertIn("FETCHCONTENT_SOURCE_DIR_SAMPLE", init.read_text())

    def test_readonly_partial_copy_failure_cleans_up_and_retry_succeeds(self):
        self.readonly_source()
        dest = self.root / "scratch/copied sources"
        original = OSError("injected later copy failure")
        real_copy = deps.shutil.copytree

        def fail(source, target, *args, **kwargs):
            result = real_copy(source, target, *args, **kwargs)
            if Path(target) == dest / "sample":
                raise original
            return result

        with patch.object(deps.shutil, "copytree", side_effect=fail):
            with self.assertRaises(OSError) as caught:
                self.stage()
        self.assertIs(caught.exception, original)
        self.assertFalse(dest.exists())
        self.stage()
        self.assertTrue((self.root / "scratch/initial cache.cmake").is_file())

    def test_readonly_symlink_failure_cleanup_preserves_targets_and_retry_succeeds(self):
        directory, content = self.readonly_source()
        mode = directory.stat().st_mode
        # Source snapshots must be valid on Windows, where all reparse points
        # are refused. Inject hostile links into the partial copy instead.
        self.write_lock()
        modes = [path.stat().st_mode for path in (directory, content)]
        real_copy = deps.shutil.copytree
        real_chmod, real_unlink, real_rmdir = os.chmod, os.unlink, os.rmdir
        real_lstat, real_symlink = os.lstat, os.symlink

        for phase, deny_links in ((phase, denied) for phase in ("publication", "copy") for denied in (False, True)):
            with self.subTest(phase=phase, deny_links=deny_links):
                fixture = phase + (" denied links" if deny_links else "")
                dest = self.root / "scratch" / fixture
                init = self.root / "scratch" / (fixture + ".cmake")
                original = OSError("injected " + phase + " failure")
                cleaning = False
                retried_links = set()
                simulated_links = {}

                def lstat(path, *args, **kwargs):
                    info = real_lstat(path, *args, **kwargs)
                    if Path(path) in simulated_links:
                        fields = {name: getattr(info, name) for name in dir(info) if name.startswith("st_")}
                        fields["st_mode"] = stat.S_IFLNK | 0o777
                        return SimpleNamespace(**fields)
                    return info

                def symlink(target, link, *, target_is_directory):
                    try:
                        if deny_links:
                            raise PermissionError(errno.EPERM, "symlink creation denied")
                        real_symlink(target, link, target_is_directory=target_is_directory)
                    except OSError as error:
                        if (error.errno not in (errno.EPERM, errno.EACCES, errno.ENOSYS, errno.EOPNOTSUPP)
                                and getattr(error, "winerror", None) not in (1, 50, 1314)):
                            raise
                        # A stand-in directory entry plus lstat metadata models
                        # no-follow cleanup even without symlink privileges.
                        link.write_text("link stand-in")
                        simulated_links[link] = target

                def fail():
                    nonlocal cleaning
                    copied = dest / "sample/readonly directory"
                    self.assertEqual((copied / content.name).read_text(), "readonly content\n")
                    # Model links pointing outside the partial stage. Cleanup
                    # must neither traverse them nor chmod their cache targets.
                    real_chmod(copied, mode | stat.S_IWUSR)
                    for name, target in (("file link", content), ("directory link", directory)):
                        link = copied / name
                        symlink(target, link, target_is_directory=target == directory)
                        self.assertTrue(stat.S_ISLNK(os.lstat(link).st_mode))
                    real_chmod(copied, mode)
                    cleaning = True
                    raise original

                def copy(source, target, *args, **kwargs):
                    result = real_copy(source, target, *args, **kwargs)
                    if phase == "copy" and Path(target) == dest / "sample":
                        fail()
                    return result

                def publish(source, target):
                    self.assertEqual(Path(target), init)
                    fail()

                def chmod(path, *args, **kwargs):
                    if cleaning:
                        self.assertFalse(stat.S_ISLNK(os.lstat(path).st_mode))
                        self.assertFalse(Path(path).resolve().is_relative_to(self.cache))
                    return real_chmod(path, *args, **kwargs)

                def remove(function, path, *args, **kwargs):
                    name = Path(path).name
                    if cleaning and name in {"file link", "directory link"} and name not in retried_links:
                        retried_links.add(name)
                        raise PermissionError("injected read-only link deletion")
                    return function(path, *args, **kwargs)

                with patch.object(deps.shutil, "copytree", side_effect=copy), \
                        patch.object(deps.os, "link", side_effect=publish), \
                        patch.object(deps.os, "lstat", side_effect=lstat), \
                        patch.object(Path, "lstat", lstat), \
                        patch.object(deps.os, "chmod", side_effect=chmod), \
                        patch.object(deps.os, "unlink", side_effect=lambda *a, **kw: remove(real_unlink, *a, **kw)), \
                        patch.object(deps.os, "rmdir", side_effect=lambda *a, **kw: remove(real_rmdir, *a, **kw)):
                    with self.assertRaises(OSError) as caught:
                        self.stage(dest, init)
                self.assertIs(caught.exception, original)
                self.assertFalse(dest.exists(), getattr(original, "__notes__", ()))
                self.assertFalse(init.exists())
                self.assertEqual(list(init.parent.glob(init.name + ".*.tmp")), [])
                self.assertEqual(retried_links, {"file link", "directory link"})
                self.assertEqual([path.stat().st_mode for path in (directory, content)], modes)
                self.assertEqual(content.read_text(), "readonly content\n")
                if deny_links:
                    self.assertEqual({path.name for path in simulated_links}, retried_links)
                self.stage(dest, init)
                copied = dest / "sample/readonly directory"
                self.assertFalse((copied / "file link").exists())
                self.assertFalse((copied / "directory link").exists())
                self.assertEqual((copied / content.name).read_text(), "readonly content\n")
                self.assertEqual(deps.source_digest(dest / "sample"), deps.source_digest(self.source))
                self.assertEqual([path.stat().st_mode for path in (directory, content)], modes)
                self.assertIn("FETCHCONTENT_SOURCE_DIR_SAMPLE", init.read_text())

    def test_stage_cleanup_failures_preserve_original_exception(self):
        original = OSError("injected cache publication failure")
        real_unlink = Path.unlink
        real_rmtree = deps.shutil.rmtree
        dest = self.root / "scratch/copied sources"

        for stage_failure, cache_failure in ((True, False), (False, True), (True, True)):
            with self.subTest(stage_failure=stage_failure, cache_failure=cache_failure):
                attempts = []

                def remove(*args, **kwargs):
                    if stage_failure:
                        raise PermissionError("injected stage cleanup failure")
                    return real_rmtree(*args, **kwargs)

                def unlink(path, *args, **kwargs):
                    if path.name.startswith("initial cache.cmake."):
                        attempts.append(path)
                        if cache_failure:
                            raise PermissionError("injected temporary cache cleanup failure")
                    return real_unlink(path, *args, **kwargs)

                with patch.object(deps.os, "link", side_effect=original), \
                        patch.object(deps.shutil, "rmtree", side_effect=remove), \
                        patch.object(Path, "unlink", unlink):
                    with self.assertRaises(OSError) as caught:
                        self.stage()
                self.assertIs(caught.exception, original)
                self.assertEqual(len(attempts), 1)
                self.assertEqual(dest.exists(), stage_failure)
                self.assertEqual(attempts[0].exists(), cache_failure)
                if dest.exists():
                    real_rmtree(dest)
                attempts[0].unlink(missing_ok=True)

    def test_concurrent_stages_publish_exactly_one_complete_initial_cache(self):
        self.readonly_source()
        initial_digest = deps.source_digest(self.source)
        initial_lock = self.lock.read_bytes()
        init = self.root / "scratch/shared initial cache.cmake"
        destinations = [self.root / "scratch" / name for name in ("first stage", "second stage")]
        ready = threading.Barrier(2)
        reached = set()
        outcomes = [None, None]
        published = []
        real_temporary, real_link = deps.tempfile.NamedTemporaryFile, deps.os.link

        @contextmanager
        def complete_candidate(*args, **kwargs):
            with real_temporary(*args, **kwargs) as stream:
                yield stream
            # Both freshness checks and complete closed writes precede either
            # native publication. The same boundary also reaches old rename code.
            reached.add(threading.get_ident())
            ready.wait()

        def publish(source, target):
            result = real_link(source, target)
            published.append(Path(target).read_bytes())
            return result

        def run(index):
            try:
                self.stage(destinations[index], init)
                outcomes[index] = True
            except BaseException as error:
                outcomes[index] = error
                if threading.get_ident() not in reached:
                    ready.abort()

        with patch.object(deps.tempfile, "NamedTemporaryFile", side_effect=complete_candidate), \
                patch.object(deps.os, "link", side_effect=publish):
            threads = [threading.Thread(target=run, args=(index,)) for index in range(2)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
        winners = [index for index, result in enumerate(outcomes) if result is True]
        self.assertEqual(len(winners), 1, outcomes)
        winner = winners[0]
        loser = 1 - winner
        self.assertIsInstance(outcomes[loser], FileExistsError)
        self.assertEqual(outcomes[loser].errno, errno.EEXIST)
        self.assertEqual(published, [init.read_bytes()])
        self.assertTrue(destinations[winner].is_dir())
        self.assertFalse(destinations[loser].exists())
        self.assertIn(deps.cmake_string(str(destinations[winner] / "sample")), init.read_text())
        self.assertNotIn(deps.cmake_string(str(destinations[loser] / "sample")), init.read_text())
        self.assertEqual(deps.source_digest(destinations[winner] / "sample"), initial_digest)
        self.assertEqual(deps.source_digest(self.source), initial_digest)
        self.assertEqual(self.lock.read_bytes(), initial_lock)
        self.assertEqual(list(init.parent.glob(init.name + ".*.tmp")), [])
        self.assertEqual(init.stat().st_nlink, 1)

    def test_late_initial_cache_occupants_are_never_replaced_or_removed(self):
        real_verify, real_link = deps.verify, deps.os.link
        foreign = self.base / "foreign cache"
        foreign.write_bytes(b"retained foreign configuration\n")
        for kind in ("file", "directory", "hardlink"):
            with self.subTest(kind=kind):
                dest = self.root / "scratch" / (kind + " stage")
                init = self.root / "scratch" / (kind + " init.cmake")
                attempts = []

                def verified(*args):
                    lock = real_verify(*args)
                    init.parent.mkdir(parents=True, exist_ok=True)
                    if kind == "directory":
                        init.mkdir()
                        (init / "retained").write_bytes(foreign.read_bytes())
                    elif kind == "hardlink":
                        real_link(foreign, init)
                    else:
                        init.write_bytes(foreign.read_bytes())
                    return lock

                def publish(source, target):
                    try:
                        return real_link(source, target)
                    except OSError as error:
                        attempts.append(error)
                        raise

                with patch.object(deps, "verify", side_effect=verified), \
                        patch.object(deps.os, "link", side_effect=publish):
                    with self.assertRaises(OSError) as caught:
                        self.stage(dest, init)
                self.assertEqual(attempts, [caught.exception])
                if kind != "directory":
                    self.assertIsInstance(caught.exception, FileExistsError)
                    self.assertEqual(caught.exception.errno, errno.EEXIST)
                self.assertFalse(dest.exists())
                content = init / "retained" if kind == "directory" else init
                self.assertEqual(content.read_bytes(), foreign.read_bytes())
                if kind == "hardlink":
                    self.assertTrue(os.path.samestat(init.lstat(), foreign.lstat()))
                self.assertEqual(list(init.parent.glob(init.name + ".*.tmp")), [])

    def test_native_missing_candidate_failure_rolls_back_stage_and_allows_retry(self):
        init = self.root / "scratch/initial cache.cmake"
        dest = self.root / "scratch/copied sources"
        real_link = deps.os.link
        failures = []

        def publish(source, target):
            Path(source).unlink()
            try:
                return real_link(source, target)
            except OSError as error:
                failures.append(error)
                raise

        with patch.object(deps.os, "link", side_effect=publish):
            with self.assertRaises(FileNotFoundError) as caught:
                self.stage()
        self.assertEqual(failures, [caught.exception])
        self.assertEqual(caught.exception.errno, errno.ENOENT)
        self.assertFalse(dest.exists())
        self.assertFalse(init.exists())
        self.assertEqual(list(init.parent.glob(init.name + ".*.tmp")), [])
        self.stage()
        self.assertIn(deps.cmake_string(str(dest / "sample")), init.read_text())
        self.assertTrue((dest / "sample/input with spaces.h").is_file())

    def test_completed_publication_followed_by_exception_retains_configuration_and_stage(self):
        real_link, real_unlink = deps.os.link, Path.unlink
        for number, (error_type, cleanup_failure) in enumerate(
                (error_type, cleanup_failure) for error_type in (OSError, KeyboardInterrupt)
                for cleanup_failure in (False, True)):
            with self.subTest(error=error_type.__name__, cleanup_failure=cleanup_failure):
                original = error_type("completed link")
                dest = self.root / "scratch" / (str(number) + " retained stage")
                init = self.root / "scratch" / (str(number) + " retained.cmake")
                published, attempts = [], []

                def publish(source, target):
                    real_link(source, target)
                    published.append(Path(target).read_bytes())
                    raise original

                def unlink(path, *args, **kwargs):
                    if cleanup_failure and path.name.startswith(init.name + "."):
                        attempts.append(path)
                        raise PermissionError("cleanup after completed link failed")
                    return real_unlink(path, *args, **kwargs)

                with patch.object(deps.os, "link", side_effect=publish), \
                        patch.object(Path, "unlink", unlink):
                    with self.assertRaises(error_type) as caught:
                        self.stage(dest, init)
                self.assertIs(caught.exception, original)
                self.assertEqual(published, [init.read_bytes()])
                self.assertIn(deps.cmake_string(str(dest / "sample")), init.read_text())
                self.assertEqual(deps.source_digest(dest / "sample"), deps.source_digest(self.source))
                self.assertEqual(len(attempts), int(cleanup_failure))
                if cleanup_failure:
                    self.assertEqual(list(init.parent.glob(init.name + ".*.tmp")), attempts)
                    if sys.version_info >= (3, 11):
                        self.assertTrue(any("initial cache cleanup failed" in note for note in original.__notes__))
                    attempts[0].unlink()
                self.assertEqual(init.stat().st_nlink, 1)
                self.assertEqual(list(init.parent.glob(init.name + ".*.tmp")), [])

    def test_candidate_cleanup_errors_after_publication_keep_stage_and_original_error(self):
        real_unlink = Path.unlink
        for failure in ("once", "always", "removed then raised"):
            with self.subTest(failure=failure):
                dest = self.root / "scratch" / (failure + " stage")
                init = self.root / "scratch" / (failure + ".cmake")
                original = PermissionError("candidate cleanup failed")
                attempts = []

                def unlink(path, *args, **kwargs):
                    if path.name.startswith(init.name + "."):
                        attempts.append(path)
                        if len(attempts) == 1 or failure == "always":
                            self.assertTrue(init.is_file())
                            if failure == "removed then raised":
                                real_unlink(path, *args, **kwargs)
                            raise original
                    return real_unlink(path, *args, **kwargs)

                with patch.object(Path, "unlink", unlink):
                    with self.assertRaises(PermissionError) as caught:
                        self.stage(dest, init)
                self.assertIs(caught.exception, original)
                self.assertEqual(len(attempts), 2)
                self.assertIn(deps.cmake_string(str(dest / "sample")), init.read_text())
                self.assertEqual(deps.source_digest(dest / "sample"), deps.source_digest(self.source))
                self.assertEqual(attempts[0].exists(), failure == "always")
                if failure == "always" and sys.version_info >= (3, 11):
                    self.assertTrue(any("initial cache cleanup failed" in note for note in original.__notes__))
                attempts[0].unlink(missing_ok=True)
                self.assertEqual(init.stat().st_nlink, 1)

    def test_uncertain_publication_retains_stage_without_claiming_an_occupant(self):
        real_link, real_lstat = deps.os.link, Path.lstat
        # Every lookup failure and missing identity is exercised with a real
        # published file and with a genuine native EEXIST foreign occupant.
        for number, (outcome, metadata) in enumerate(
                (outcome, metadata) for outcome in ("published", "foreign")
                for metadata in ("inode", "device", "candidate inode", "candidate device", "denied", "interrupted")):
            with self.subTest(outcome=outcome, metadata=metadata):
                dest = self.root / "scratch" / (str(number) + " uncertain stage")
                init = self.root / "scratch" / (str(number) + " uncertain.cmake")
                original = OSError("uncertain publication")
                observing = False
                expected = []

                def publish(source, target):
                    nonlocal observing, original
                    if outcome == "foreign":
                        Path(target).write_bytes(b"unknown owner\n")
                    try:
                        real_link(source, target)
                    except OSError as error:
                        original = error
                    expected.append(Path(target).read_bytes())
                    observing = True
                    raise original

                def metadata_for(path, *args, **kwargs):
                    if metadata.startswith("candidate ") and path.name.startswith(init.name + "."):
                        info = real_lstat(path, *args, **kwargs)
                        fields = {key: getattr(info, key) for key in dir(info) if key.startswith("st_")}
                        fields["st_ino" if metadata.endswith("inode") else "st_dev"] = 0
                        return SimpleNamespace(**fields)
                    if path == init and observing:
                        if metadata == "denied":
                            raise PermissionError("lookup denied")
                        if metadata == "interrupted":
                            raise KeyboardInterrupt("lookup interrupted")
                        info = real_lstat(path, *args, **kwargs)
                        fields = {key: getattr(info, key) for key in dir(info) if key.startswith("st_")}
                        fields["st_ino" if metadata == "inode" else "st_dev"] = 0
                        return SimpleNamespace(**fields)
                    return real_lstat(path, *args, **kwargs)

                with patch.object(deps.os, "link", side_effect=publish), \
                        patch.object(Path, "lstat", metadata_for):
                    with self.assertRaises(OSError) as caught:
                        self.stage(dest, init)
                self.assertIs(caught.exception, original)
                self.assertEqual(expected, [init.read_bytes()])
                self.assertEqual(deps.source_digest(dest / "sample"), deps.source_digest(self.source))
                if outcome == "published":
                    self.assertIn(deps.cmake_string(str(dest / "sample")), init.read_text())
                else:
                    self.assertEqual(init.read_bytes(), b"unknown owner\n")
                if sys.version_info >= (3, 11):
                    self.assertTrue(any("stage retained:" in note for note in original.__notes__))
                self.assertEqual(list(init.parent.glob(init.name + ".*.tmp")), [])

    def test_candidate_identity_lookup_failure_rolls_back_before_publication(self):
        real_lstat = Path.lstat
        for missing in (False, True):
            with self.subTest(missing=missing):
                dest = self.root / "scratch" / (str(missing) + " metadata stage")
                init = self.root / "scratch" / (str(missing) + " metadata.cmake")
                original = FileNotFoundError("candidate disappeared") if missing else PermissionError("candidate metadata denied")
                inspected = []

                def metadata_for(path, *args, **kwargs):
                    if path.name.startswith(init.name + "."):
                        inspected.append(path)
                        if missing:
                            path.unlink()
                        raise original
                    return real_lstat(path, *args, **kwargs)

                with patch.object(Path, "lstat", metadata_for), \
                        patch.object(deps.os, "link", wraps=deps.os.link) as publish:
                    with self.assertRaises(type(original)) as caught:
                        self.stage(dest, init)
                self.assertIs(caught.exception, original)
                self.assertEqual(len(inspected), 1)
                publish.assert_not_called()
                self.assertFalse(dest.exists())
                self.assertFalse(init.exists())
                self.assertEqual(list(init.parent.glob(init.name + ".*.tmp")), [])

    def test_unpublished_interruption_rolls_back_and_preserves_original_error(self):
        original = KeyboardInterrupt("before native link")
        with patch.object(deps.os, "link", side_effect=original):
            with self.assertRaises(KeyboardInterrupt) as caught:
                self.stage()
        self.assertIs(caught.exception, original)
        self.assertFalse((self.root / "scratch/copied sources").exists())
        init = self.root / "scratch/initial cache.cmake"
        self.assertFalse(init.exists())
        self.assertEqual(list(init.parent.glob(init.name + ".*.tmp")), [])

    def test_cli_late_publication_collision_returns_failure_and_preserves_occupant(self):
        init = self.root / "scratch/initial cache.cmake"
        real_verify = deps.verify

        def verified(*args):
            result = real_verify(*args)
            init.parent.mkdir(parents=True, exist_ok=True)
            init.write_bytes(b"CLI concurrent winner\n")
            return result

        diagnostic = io.StringIO()
        with patch.object(deps, "verify", side_effect=verified), redirect_stderr(diagnostic):
            result = deps.main(["stage", "--root", str(self.root), "--cache", str(self.cache),
                                "--lock", str(self.lock), "--dest", "scratch/copied sources",
                                "--init", "scratch/initial cache.cmake"])
        self.assertEqual(result, 1)
        self.assertIn("offline dependencies:", diagnostic.getvalue())
        self.assertEqual(init.read_bytes(), b"CLI concurrent winner\n")
        self.assertFalse((self.root / "scratch/copied sources").exists())
        self.assertEqual(list(init.parent.glob(init.name + ".*.tmp")), [])

    def test_stage_never_writes_initial_cache_inside_source_cache(self):
        local_cache = self.root / "scratch/source cache"
        local_cache.parent.mkdir(exist_ok=True)
        self.cache.rename(local_cache)
        with self.assertRaisesRegex(deps.DependencyError, "separate"):
            deps.stage(self.root, local_cache, self.lock,
                       self.root / "scratch/staged", local_cache / "initial.cmake")
        self.assertFalse((local_cache / "initial.cmake").exists())

    def test_moving_cache_during_copy_removes_partial_stage(self):
        real_copy = deps.shutil.copytree
        def copy(*args, **kwargs):
            result = real_copy(*args, **kwargs)
            (Path(result) / "extra").write_text("race")
            return result
        with patch.object(deps.shutil, "copytree", side_effect=copy):
            with self.assertRaisesRegex(deps.DependencyError, "during copy"):
                self.stage()
        self.assertFalse((self.root / "scratch/copied sources").exists())
        self.assertFalse((self.root / "scratch/initial cache.cmake").exists())

    def test_submodule_identity_and_uninitialized_submodules_refused(self):
        self.data["dependencies"]["sample"]["submodules"] = {"nested": "b" * 40}
        self.save_lock()
        with self.assertRaisesRegex(deps.DependencyError, "submodule revisions"):
            self.verify()
        self.git_mock.side_effect = None
        self.git_mock.return_value = "160000 commit " + "b" * 40 + "\tnested\0"
        with self.assertRaisesRegex(deps.DependencyError, "incomplete submodules"):
            deps.submodules(self.source, self.cache)

    def test_submodule_paths_with_spaces_and_nested_paths(self):
        nested = self.source / "nested path"
        child = nested / "child"
        (child / ".git").mkdir(parents=True)
        (nested / ".git").mkdir()
        def tree_git(source, *args, **kwargs):
            if args == ("ls-tree", "-rz", "HEAD"):
                return {self.source: "160000 commit " + "b" * 40 + "\tnested path\0",
                        nested: "160000 commit " + "c" * 40 + "\tchild\0",
                        child: ""}[source]
            return {nested: "b" * 40, child: "c" * 40}[source]
        self.git_mock.side_effect = tree_git
        self.assertEqual(deps.submodules(self.source, self.cache),
                         {"nested path": "b" * 40, "nested path/child": "c" * 40})

    def test_submodule_tree_rejects_malformed_and_truncated_records(self):
        self.git_mock.side_effect = None
        for tree in ("0", "160000 commit " + "b" * 40 + "\tnested", "bad\0",
                     "160000 blob " + "b" * 40 + "\tnested\0"):
            with self.subTest(tree=tree):
                self.git_mock.return_value = tree
                with self.assertRaises(deps.DependencyError):
                    deps.submodules(self.source, self.cache)

    def test_submodule_gitlink_cannot_escape_checkout(self):
        self.git_mock.side_effect = None
        self.git_mock.return_value = "160000 commit " + "b" * 40 + "\t../outside\0"
        with self.assertRaises(deps.DependencyError):
            deps.submodules(self.source, self.cache)

    def test_glad_requires_bundled_generator_and_specification_inputs(self):
        with self.assertRaisesRegex(deps.DependencyError, "GLAD input"):
            deps.glad_inputs(self.source)
        for name in ("glad/__main__.py", "glad/files/gl.xml", "glad/files/khrplatform.h"):
            path = self.source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("bundled")
        deps.glad_inputs(self.source)

    def test_snapshot_fetches_pins_shallow_and_includes_recursive_submodules(self):
        fresh = self.base / "fresh snapshot"
        def snapshot_git(source, *args, **kwargs):
            if args[:1] == ("checkout",):
                (source / ".git").mkdir()
                (source / "source").write_text("complete")
            if args == ("rev-parse", "--verify", "HEAD^0"):
                return self.commit
            return ""
        self.git_mock.side_effect = snapshot_git
        deps.snapshot(self.root, fresh, self.lock)
        calls = [call.args[1:] for call in self.git_mock.call_args_list]
        self.assertIn(("fetch", "--depth=1", "--no-tags", "origin", "v1"), calls)
        self.assertIn(("submodule", "update", "--init", "--recursive", "--depth=1"), calls)
        for call in self.git_mock.call_args_list:
            self.assertEqual(call.kwargs["cache"], fresh)
            self.assertEqual(call.kwargs.get("network", False),
                             call.args[1] in {"fetch", "submodule"})
        data = json.loads(self.lock.read_text())
        self.assertEqual(data["status"], "resolved")
        self.assertEqual(data["dependencies"]["sample"]["commit"], self.commit)
        with self.assertRaisesRegex(deps.DependencyError, "empty cache"):
            deps.snapshot(self.root, fresh, self.lock)

    def test_failed_snapshot_does_not_publish_resolved_lock(self):
        self.lock.write_text(json.dumps(deps.unresolved_lock(self.root)))
        self.git_mock.side_effect = subprocess.CalledProcessError(1, "git")
        with self.assertRaises(subprocess.CalledProcessError):
            deps.snapshot(self.root, self.base / "failed", self.lock)
        self.assertEqual(json.loads(self.lock.read_text())["status"], "unresolved")

    def test_cmake_quotes_prevent_interpolation_or_delimiter_escape(self):
        value = 'path with ; ${variable} "]=] text'
        quoted = deps.cmake_string(value)
        self.assertTrue(quoted.startswith("[==["))
        self.assertEqual(quoted[4:-4], value)

    def test_cli_failure_returns_nonzero(self):
        self.lock.write_text(json.dumps(deps.unresolved_lock(self.root)))
        self.assertEqual(deps.main(["stage", "--root", str(self.root), "--cache", str(self.cache), "--lock", str(self.lock)]), 1)

    def assert_cli_refused(self, command):
        diagnostic = io.StringIO()
        with redirect_stderr(diagnostic):
            result = deps.main([command, "--root", str(self.root),
                                "--cache", str(self.cache), "--lock", str(self.lock)])
        self.assertEqual(result, 1)
        self.assertTrue(diagnostic.getvalue().startswith("offline dependencies: "))
        self.assertNotIn("Traceback", diagnostic.getvalue())

    def test_source_cycle_and_io_errors_refuse_hash_verify_stage_and_cli_without_writes(self):
        # Model the link at the filesystem boundary on every host. Windows
        # requires no symlink privilege and still reaches the hashing branch.
        alias = self.source / "alias"
        alias.write_text("placeholder")
        lstat, readlink, resolve = Path.lstat, os.readlink, Path.resolve

        def link_stat(path, *args, **kwargs):
            info = lstat(path, *args, **kwargs)
            if path == alias:
                return SimpleNamespace(st_mode=stat.S_IFLNK | 0o777)
            return info

        before = self.fixture_state()
        for failure in (RuntimeError("source cycle"), OSError(errno.ELOOP, "source cycle"),
                        PermissionError(errno.EACCES, "source denied"), OSError(errno.EIO, "partial lookup")):
            expected = deps.DependencyError if isinstance(failure, RuntimeError) or failure.errno == errno.ELOOP else OSError
            for target in (alias, self.source):
                def failed_resolve(path):
                    if path == target:
                        raise failure
                    return resolve(path)

                with self.subTest(target=target, failure=failure), patch.object(Path, "lstat", link_stat), \
                        patch.object(os, "readlink", lambda path: "alias" if Path(path) == alias else readlink(path)), \
                        patch.object(Path, "resolve", failed_resolve):
                    for action in (lambda: deps.source_digest(self.source), self.verify, self.stage):
                        with self.subTest(action=action), self.assertRaises(expected):
                            action()
                    for command in ("verify", "stage"):
                        with self.subTest(command=command):
                            self.assert_cli_refused(command)
                self.assertEqual(self.fixture_state(), before)

    def test_resolution_siblings_and_cli_refuse_cycles_and_io_errors_without_writes(self):
        resolve = Path.resolve
        scratch = self.root / "scratch"
        dest, init = scratch / "copied sources", scratch / "initial cache.cmake"
        cases = ((self.source, self.verify), (self.cache, self.verify),
                 (self.source / ".git", lambda: deps.git_metadata(self.source, self.cache)),
                 (self.cache, self.stage), (scratch, self.stage), (self.root, self.stage),
                 (dest, self.stage), (init, self.stage))
        before = self.fixture_state()
        for failure in (RuntimeError("cyclic lookup"), OSError(errno.ELOOP, "cyclic lookup"),
                        PermissionError(errno.EACCES, "lookup denied"), OSError(errno.EIO, "partial lookup")):
            expected = deps.DependencyError if isinstance(failure, RuntimeError) or failure.errno == errno.ELOOP else OSError
            for target, action in cases:
                def failed_resolve(path):
                    if path == target:
                        raise failure
                    return resolve(path)

                with self.subTest(target=target, failure=failure), patch.object(Path, "resolve", failed_resolve):
                    with self.assertRaises(expected):
                        action()
                self.assertEqual(self.fixture_state(), before)
            for target in (self.root, self.cache, self.source, self.source / ".git", scratch,
                           scratch / "offline-deps", scratch / "offline-init.cmake"):
                for command in ("verify", "stage"):
                    if command == "verify" and target.is_relative_to(scratch):
                        continue

                    def failed_resolve(path):
                        if path == target:
                            raise failure
                        return resolve(path)

                    with self.subTest(command=command, target=target, failure=failure), \
                            patch.object(Path, "resolve", failed_resolve):
                        self.assert_cli_refused(command)
                    self.assertEqual(self.fixture_state(), before)

    def test_source_links_keep_exact_source_boundary_and_digest_encoding(self):
        # Pin bytes independently of Windows text newline translation.
        (self.source / "input with spaces.h").write_bytes(b"source content\n")
        alias = self.source / "alias"
        alias.write_text("placeholder")
        lstat, readlink, resolve, exists = Path.lstat, os.readlink, Path.resolve, Path.exists

        def link_stat(path, *args, **kwargs):
            return SimpleNamespace(st_mode=stat.S_IFLNK | 0o777) if path == alias else lstat(path, *args, **kwargs)

        # Equality with the source root is allowed for source links, whereas
        # Git metadata must be strictly inside its cache. A sibling source
        # inside the same cache is still outside this source's boundary.
        cases = ((".", self.source, True, None),
                 ("input with spaces.h", self.source / "input with spaces.h", True, None),
                 ("../sibling/input.h", self.cache / "sibling/input.h", True, "symlink escapes"),
                 (str(self.source / "input with spaces.h"), self.source / "input with spaces.h", True, "symlink escapes"),
                 ("missing.h", self.source / "missing.h", False, "dangling source symlink"))
        before = self.fixture_state()
        for target, resolved, present, refusal in cases:
            with self.subTest(target=target), patch.object(Path, "lstat", link_stat), \
                    patch.object(os, "readlink", lambda path: target if Path(path) == alias else readlink(path)), \
                    patch.object(Path, "resolve", lambda path: resolved if path == alias else resolve(path)), \
                    patch.object(Path, "exists", lambda path: present if path == alias else exists(path)):
                if refusal:
                    with self.assertRaisesRegex(deps.DependencyError, refusal):
                        deps.source_digest(self.source)
                elif target == ".":
                    self.assertEqual(deps.source_digest(self.source),
                                     "2a634b0332bf0a6919181fec1196c7c6263f044965842c95219fbdfb88c9e343")
                else:
                    self.assertRegex(deps.source_digest(self.source), r"^[0-9a-f]{64}$")
            self.assertEqual(self.fixture_state(), before)

    def test_real_source_cycle_and_cli_root_cycle_refuse_without_writes(self):
        alias = self.source / "alias"
        root_alias = self.base / "root-cycle"
        if os.name == "posix":
            alias.symlink_to("alias")
            root_alias.symlink_to("root-cycle")
            for action in (lambda: deps.source_digest(self.source), self.verify, self.stage):
                with self.subTest(action=action), self.assertRaises(deps.DependencyError):
                    action()
            entrypoint = [str(ROOT / "tools/ci/offline_deps.py")]
        else:
            alias.write_text("placeholder")
            # Install mocks inside the child process, rather than assuming
            # parent mocks cross subprocess boundaries. This exercises the
            # CLI on Windows even when symlink creation is denied.
            driver = """import importlib.util, pathlib, os, stat, sys
from types import SimpleNamespace
from unittest.mock import patch
spec = importlib.util.spec_from_file_location('deps', sys.argv[1])
deps = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deps)
alias, root_alias = map(pathlib.Path, sys.argv[2:4])
resolve, lstat, readlink = pathlib.Path.resolve, pathlib.Path.lstat, os.readlink
def cyclic(path):
    if path in (alias, root_alias):
        raise RuntimeError('cyclic path')
    return resolve(path)
def link_stat(path, *args, **kwargs):
    return SimpleNamespace(st_mode=stat.S_IFLNK | 0o777) if path == alias else lstat(path, *args, **kwargs)
with patch.object(pathlib.Path, 'resolve', cyclic), patch.object(pathlib.Path, 'lstat', link_stat), patch.object(os, 'readlink', lambda path: 'alias' if pathlib.Path(path) == alias else readlink(path)):
    sys.exit(deps.main(sys.argv[4:]))
"""
            entrypoint = ["-c", driver, str(ROOT / "tools/ci/offline_deps.py"), str(alias), str(root_alias)]
        before = self.fixture_state()
        for root in (self.root, root_alias):
            for command in ("verify", "stage"):
                with self.subTest(root=root, command=command):
                    result = subprocess.run(
                        [sys.executable, "-I", "-B", *entrypoint, command,
                         "--root", str(root), "--cache", str(self.cache), "--lock", str(self.lock)],
                        capture_output=True, text=True, check=False)
                    self.assertEqual(result.returncode, 1)
                    self.assertTrue(result.stderr.startswith("offline dependencies: "), result.stderr)
                    self.assertNotIn("Traceback", result.stderr)
                    self.assertEqual(result.stdout, "")
                    self.assertEqual(self.fixture_state(), before)


def _snapshot_paths(root):
    """Enumerate fresh names in independently owned, idle test fixtures."""
    # Snapshot callers observe only the completed mapping, between synchronous
    # actions. Walk visits ordinary directories once without following links.
    # Retain the original traversal on Python versions without Path.walk.
    walk = getattr(root, "walk", None)
    if walk is None:
        yield from root.rglob("*")
        return
    for directory, directories, files in walk(follow_symlinks=False):
        for names in (directories, files):
            for name in names:
                yield directory / name


class CachedGitSafetyTests(unittest.TestCase):
    """Use real local Git repositories; no transports or native tools run."""

    @classmethod
    def setUpClass(cls):
        temporary = tempfile.TemporaryDirectory(prefix="cached git template ")
        cls.addClassCleanup(temporary.cleanup)
        cls.template_cache = Path(temporary.name).resolve()
        cls.template_source = cls.template_cache / "sample"
        cls.template_source.mkdir()
        cls.initialize(cls.template_source, cls.template_cache)
        cls.template_identity = {
            "commit": deps.head(cls.template_source, cls.template_cache),
            "sha256": deps.source_digest(cls.template_source),
            "submodules": deps.submodules(cls.template_source, cls.template_cache),
        }

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="cached git ")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name).resolve()
        self.root = self.base / "clone"
        (self.root / "vendor").mkdir(parents=True)
        (self.root / "vendor/CMakeLists.txt").write_text(
            "FetchContent_Declare(sample GIT_REPOSITORY https://example.org/sample.git GIT_TAG v1)\n")
        self.cache = self.base / "cache with spaces"
        self.source = self.cache / "sample"
        # Copy this small, fixed repository, including its real Git objects.
        # Independent files let every test mutate metadata without rebuilding
        # the same commit in five Git processes per test on loaded runners.
        shutil.copytree(self.template_source, self.source)
        self.lock = self.root / "lock.json"
        self.save_lock(self.template_identity)
        self.marker = self.base / "executed marker"
        script = self.base / "monitor script.py"
        script.write_text("import pathlib, sys\npathlib.Path(sys.argv[1]).write_text('executed')\n")
        # Git's configured commands use its shell on Windows too.
        self.command = shlex.join([Path(sys.executable).as_posix(), "-I", "-B",
                                   script.as_posix(), self.marker.as_posix()])

    def git(self, source, *args):
        return deps.git(source, *args, cache=self.cache)

    @classmethod
    def initialize(cls, source, cache):
        # Nested fixtures need the same complete initial repository as the
        # top-level copies. Reuse its immutable seed instead of starting three
        # more Git processes; subsequent gitlink and HEAD mutations stay real.
        if source != cls.template_source:
            shutil.copytree(cls.template_source, source, dirs_exist_ok=True)
            return
        deps.git(source, "init", "--quiet", cache=cache)
        (source / "input.h").write_text("content\n")
        deps.git(source, "add", "input.h", cache=cache)
        cls.commit(source, cache)

    @staticmethod
    def commit(source, cache):
        deps.git(source, "-c", "user.name=Fixture", "-c", "user.email=fixture@example.org",
                 "-c", "commit.gpgSign=false", "-c", "maintenance.auto=false",
                 "commit", "--quiet", "-m", "fixture", cache=cache)

    def test_fixture_commits_disable_maintenance_in_real_git(self):
        original_output = subprocess.check_output
        commits = []

        def output(argv, **kwargs):
            if "commit" not in argv:
                return original_output(argv, **kwargs)
            source = Path(kwargs["cwd"])
            config = source / ".git/config"
            before_config = config.read_bytes()
            before_source = deps.source_digest(source)
            trace = self.base / f"commit-trace-{len(commits)}.json"
            kwargs["env"] = dict(kwargs["env"], GIT_TRACE2_EVENT=str(trace),
                                 GIT_TRACE2_CONFIG_PARAMS="maintenance.auto")
            result = original_output(argv, **kwargs)
            self.assertEqual(config.read_bytes(), before_config)
            self.assertEqual(deps.source_digest(source), before_source)
            commits.append((source.name, trace))
            return result

        class FreshFixture(type(self)):
            pass

        with patch.object(subprocess, "check_output", side_effect=output):
            try:
                FreshFixture.setUpClass()
            finally:
                FreshFixture.doClassCleanups()
            (self.source / "input.h").write_text("changed fixture\n")
            self.git(self.source, "add", "input.h")
            self.commit(self.source, self.cache)
            # Retain the real recursive discovery, verification and refusals.
            self.test_real_recursive_gitlinks_check_initialized_heads()
        self.assertEqual([name for name, _ in commits],
                         ["sample", "sample", "nested path (v1)", "sample", "child"])
        for name, trace in commits:
            with self.subTest(repository=name, trace=trace.name):
                events = [json.loads(line) for line in trace.read_text(encoding="utf-8").splitlines()]
                self.assertTrue(any(event.get("event") == "cmd_name" and
                                    event.get("name") == "commit" for event in events), events)
                self.assertTrue(any(event.get("event") == "exit" and
                                    event.get("code") == 0 for event in events), events)
                self.assertTrue(any(event.get("event") == "def_param" and
                                    event.get("param") == "maintenance.auto" and
                                    event.get("value") == "false" for event in events), events)
                children = [event["argv"] for event in events if event.get("event") == "child_start"]
                self.assertFalse(any("maintenance" in child or "gc" in child for child in children), children)

    def save_lock(self, identity):
        lock = deps.unresolved_lock(self.root)
        lock["status"] = "resolved"
        lock["dependencies"]["sample"].update(identity)
        self.lock.write_text(json.dumps(lock))

    def write_lock(self):
        self.save_lock({
            "commit": deps.head(self.source, self.cache),
            "sha256": deps.source_digest(self.source),
            "submodules": deps.submodules(self.source, self.cache),
        })

    def actions(self):
        return (lambda: deps.verify(self.root, self.cache, self.lock),
                lambda: deps.stage(self.root, self.cache, self.lock,
                                   self.root / "scratch/staged", self.root / "scratch/init.cmake"))

    def cache_state(self):
        return {p.relative_to(self.cache).as_posix():
                os.readlink(p) if p.is_symlink() else p.read_bytes()
                for p in _snapshot_paths(self.cache) if p.is_symlink() or p.is_file()}

    def test_cache_snapshot_refreshes_full_mapping_between_actions(self):
        root = self.base / "snapshot"
        (root / ".hidden/nested").mkdir(parents=True)
        config = root / ".hidden/nested/config"
        config.write_bytes(b"config\x00\xff\r\n")
        os.link(config, root / "CONFIG~1")
        (root / "removed").write_bytes(b"remove me")
        independent = self.base / "independent snapshot"
        shutil.copytree(root, independent)
        fixture = SimpleNamespace(cache=root)
        other = SimpleNamespace(cache=independent)
        expected = {".hidden/nested/config": b"config\x00\xff\r\n",
                    "CONFIG~1": b"config\x00\xff\r\n", "removed": b"remove me"}
        initial = dict(expected)

        def compare():
            reference = {p.relative_to(root).as_posix():
                         os.readlink(p) if p.is_symlink() else p.read_bytes()
                         for p in root.rglob("*") if p.is_symlink() or p.is_file()}
            self.assertEqual(reference, expected)
            self.assertEqual(self.cache_state.__func__(fixture), reference)
            self.assertEqual(self.cache_state.__func__(other), initial)

        compare()
        (root / ".hidden/nested/added").write_bytes(b"new\x00payload")
        expected[".hidden/nested/added"] = b"new\x00payload"
        compare()
        config.write_bytes(b"changed\r\n\xff")
        expected[".hidden/nested/config"] = expected["CONFIG~1"] = b"changed\r\n\xff"
        compare()
        (root / "removed").unlink()
        del expected["removed"]
        compare()
        (root / ".hidden/nested/added").rename(root / "renamed")
        expected["renamed"] = expected.pop(".hidden/nested/added")
        compare()

    def test_cache_snapshot_names_links_without_following_cycles(self):
        # Placeholders model no-follow link classification on hosts where
        # creating a symlink requires privileges. POSIX also uses real links.
        for real_links in ((False, True) if os.name == "posix" else (False,)):
            with self.subTest(real_links=real_links):
                root = self.base / f"links-{real_links}"
                (root / "nested").mkdir(parents=True)
                (root / "nested/payload").write_bytes(b"nested\xff")
                outside = self.base / f"outside-{real_links}"
                outside.mkdir()
                (outside / "unobserved").write_bytes(b"outside")
                targets = {"file-link": ("nested/payload", False),
                           "directory-link": ("nested", True),
                           "outside-link": (str(outside), True),
                           "dangling": ("missing", False),
                           "cycle": (".", True),
                           "self-cycle": ("self-cycle", False)}
                for name, (target, directory) in targets.items():
                    link = root / name
                    if real_links:
                        link.symlink_to(target, target_is_directory=directory)
                    else:
                        link.write_bytes(b"link placeholder")
                unknown = root / "unknown-type"
                unknown.write_bytes(b"not an ordinary file")
                is_link, is_file, readlink = Path.is_symlink, Path.is_file, os.readlink

                def link_type(path):
                    if not real_links and path.parent == root and path.name in targets:
                        return True
                    return is_link(path)

                def file_type(path):
                    return False if path == unknown else is_file(path)

                def link_value(path):
                    if not real_links and path.parent == root and path.name in targets:
                        return targets[path.name][0]
                    return readlink(path)

                expected = {"nested/payload": b"nested\xff",
                            **{name: target for name, (target, _) in targets.items()}}
                with patch.object(Path, "is_symlink", link_type), \
                        patch.object(Path, "is_file", file_type), \
                        patch.object(os, "readlink", link_value):
                    reference = {p.relative_to(root).as_posix():
                                 os.readlink(p) if p.is_symlink() else p.read_bytes()
                                 for p in root.rglob("*") if p.is_symlink() or p.is_file()}
                    self.assertEqual(reference, expected)
                    self.assertEqual(self.cache_state.__func__(SimpleNamespace(cache=root)), reference)

    def test_cache_snapshot_enumerates_each_directory_at_most_once(self):
        root = self.base / "enumeration"
        directories = {root, root / ".hidden", root / "nested",
                       root / "nested/deeper", root / "empty"}
        for directory in sorted(directories):
            directory.mkdir(parents=True, exist_ok=True)
            if directory.name != "empty":
                (directory / "payload").write_bytes(b"every directory\x00\xff")
        scandir = os.scandir
        counts = []

        def scan(path):
            counts.append(Path(path))
            return scandir(path)

        with patch.object(os, "scandir", side_effect=scan):
            reference = {p.relative_to(root).as_posix():
                         os.readlink(p) if p.is_symlink() else p.read_bytes()
                         for p in root.rglob("*") if p.is_symlink() or p.is_file()}
        original_count = len(counts)
        self.assertEqual(set(counts), directories)
        counts.clear()
        with patch.object(os, "scandir", side_effect=scan):
            self.assertEqual(self.cache_state.__func__(SimpleNamespace(cache=root)), reference)
        self.assertEqual(set(counts), directories)
        # This bound does not assume a particular rglob implementation. An
        # already single-pass rglob remains valid; older Python uses fallback.
        bound = len(directories) if hasattr(Path, "walk") else original_count
        self.assertLessEqual(len(counts), bound)

    def test_cache_snapshot_preserves_scan_and_directory_type_errors(self):
        root = self.base / "scan errors"
        nested = root / "nested"
        nested.mkdir(parents=True)
        (root / "payload").write_bytes(b"root")
        (nested / "payload").write_bytes(b"nested")
        fixture = SimpleNamespace(cache=root)
        scandir = os.scandir

        def reference():
            return {p.relative_to(root).as_posix():
                    os.readlink(p) if p.is_symlink() else p.read_bytes()
                    for p in root.rglob("*") if p.is_symlink() or p.is_file()}

        def outcome(snapshot):
            try:
                return snapshot()
            except OSError as error:
                self.assertIs(error, failure)
                return error

        for failure in (PermissionError(errno.EACCES, "denied"),
                        FileNotFoundError(errno.ENOENT, "gone"),
                        OSError(errno.EIO, "unreadable")):
            for failed_path in (root, nested):
                def scan(path):
                    if Path(path) == failed_path:
                        raise failure
                    return scandir(path)

                with self.subTest(failure=failure, path=failed_path), \
                        patch.object(os, "scandir", side_effect=scan):
                    expected = {} if failed_path == root else {"payload": b"root"}
                    baseline = outcome(reference)
                    self.assertTrue(baseline == expected or baseline is failure)
                    self.assertEqual(outcome(lambda: self.cache_state.__func__(fixture)), baseline)

        for failure in (PermissionError("denied type"), OSError(errno.EIO, "partial type"), None):
            class Scan:
                def __init__(self, path):
                    self.entries = scandir(path)

                def __enter__(self):
                    self.entries.__enter__()
                    return self

                def __exit__(self, *args):
                    return self.entries.__exit__(*args)

                def __iter__(self):
                    return self

                def __next__(self):
                    entry = next(self.entries)

                    def directory_type(**kwargs):
                        if Path(entry.path) == nested:
                            if failure is not None:
                                raise failure
                            return False
                        return entry.is_dir(**kwargs)

                    return SimpleNamespace(name=entry.name, path=entry.path,
                                           is_dir=directory_type, is_symlink=entry.is_symlink)

            def scan(path):
                return Scan(path)

            with self.subTest(type_failure=failure), patch.object(os, "scandir", side_effect=scan):
                baseline = outcome(reference)
                self.assertTrue(baseline == {"payload": b"root"} or baseline is failure)
                self.assertEqual(outcome(lambda: self.cache_state.__func__(fixture)), baseline)

    def test_cache_snapshot_preserves_partial_enumeration_error_visibility(self):
        root = self.base / "partial enumeration"
        root.mkdir()
        (root / "payload").write_bytes(b"payload")
        scandir = os.scandir
        failure = OSError(errno.EIO, "partial enumeration")
        attempts = []

        class Scan:
            def __init__(self, path):
                self.entries = scandir(path)
                self.started = False

            def __enter__(self):
                self.entries.__enter__()
                return self

            def __exit__(self, *args):
                return self.entries.__exit__(*args)

            def __iter__(self):
                return self

            def __next__(self):
                if self.started:
                    raise failure
                entry = next(self.entries)
                self.started = True
                return entry

        def scan(path):
            attempts.append(Path(path))
            return Scan(path)

        def reference():
            return {p.relative_to(root).as_posix():
                    os.readlink(p) if p.is_symlink() else p.read_bytes()
                    for p in root.rglob("*") if p.is_symlink() or p.is_file()}

        outcomes = []
        for snapshot in (reference, lambda: self.cache_state.__func__(SimpleNamespace(cache=root))):
            with patch.object(os, "scandir", side_effect=scan):
                try:
                    outcomes.append(("mapping", snapshot()))
                except OSError as error:
                    self.assertIs(error, failure)
                    outcomes.append(("error", error.errno))
        self.assertTrue(attempts)
        self.assertEqual(outcomes[1], outcomes[0])

    def test_cache_snapshot_retains_fresh_type_queries_and_payload_errors(self):
        root = self.base / "fresh queries"
        root.mkdir()
        entry = root / "payload"
        entry.write_bytes(b"payload")
        fixture = SimpleNamespace(cache=root)

        def reference():
            return {p.relative_to(root).as_posix():
                    os.readlink(p) if p.is_symlink() else p.read_bytes()
                    for p in root.rglob("*") if p.is_symlink() or p.is_file()}

        for snapshot in (reference, lambda: self.cache_state.__func__(fixture)):
            with patch.object(Path, "is_symlink", side_effect=(False, True)) as links, \
                    patch.object(Path, "is_file", return_value=True) as files, \
                    patch.object(os, "readlink", return_value="second fresh result") as readlink:
                self.assertEqual(snapshot(), {"payload": "second fresh result"})
                self.assertEqual(links.call_count, 2)
                self.assertEqual(files.call_count, 1)
                readlink.assert_called_once_with(entry)
            for operation in ("is_symlink", "is_file", "read_bytes"):
                failure = PermissionError(f"unreadable {operation}")
                with self.subTest(operation=operation), patch.object(Path, operation, side_effect=failure):
                    with self.assertRaises(PermissionError) as caught:
                        snapshot()
                    self.assertIs(caught.exception, failure)
            failure = OSError(errno.EIO, "unreadable link")
            with patch.object(Path, "is_symlink", return_value=True), \
                    patch.object(os, "readlink", side_effect=failure):
                with self.assertRaises(OSError) as caught:
                    snapshot()
                self.assertIs(caught.exception, failure)

    def test_cache_snapshot_legacy_traversal_preserves_empty_names_and_errors(self):
        root = self.base / "legacy traversal"
        root.mkdir()
        legacy = SimpleNamespace(rglob=root.rglob)
        self.assertEqual(list(_snapshot_paths(legacy)), [])
        self.assertEqual(self.cache_state.__func__(SimpleNamespace(cache=root)), {})
        (root / ".hidden").write_bytes(b"payload")
        self.assertEqual(set(_snapshot_paths(legacy)), set(root.rglob("*")))
        failure = PermissionError("unreadable legacy traversal")
        with patch.object(legacy, "rglob", side_effect=failure):
            with self.assertRaises(PermissionError) as caught:
                list(_snapshot_paths(legacy))
        self.assertIs(caught.exception, failure)

    def assert_refused_without_writes(self):
        before = self.cache_state()
        lock = self.lock.read_bytes()
        for action in self.actions():
            with self.assertRaises((deps.DependencyError, subprocess.CalledProcessError)):
                action()
            self.assertFalse(self.marker.exists())
            self.assertEqual(self.cache_state(), before)
            self.assertEqual(self.lock.read_bytes(), lock)
            self.assertFalse((self.root / "scratch/staged").exists())
            self.assertFalse((self.root / "scratch/init.cmake").exists())

    def test_fixture_copies_are_independent_without_git_setup(self):
        # This fails if per-test setup starts Git again. The actual verifier
        # and stager below use real Git after the setup-only guard is removed.
        fresh = type(self)()
        self.addCleanup(fresh.doCleanups)
        with patch.object(deps.subprocess, "check_output",
                          side_effect=AssertionError("fixture setup started Git")):
            fresh.setUp()
        original_config = (self.source / ".git/config").read_bytes()
        original_lock = self.lock.read_bytes()
        self.assertEqual(fresh.lock.read_bytes(), original_lock)
        (self.source / "input.h").write_text("changed source\n")
        (self.source / ".git/config").write_text('[include]\npath = missing\n')
        (self.source / ".git/HEAD").unlink()
        self.lock.write_text("invalid lock")
        for source in (fresh.source, self.template_source):
            with self.subTest(source=source):
                self.assertEqual((source / "input.h").read_text(), "content\n")
                self.assertEqual((source / ".git/config").read_bytes(), original_config)
                self.assertTrue((source / ".git/HEAD").is_file())
        self.assertEqual(fresh.lock.read_bytes(), original_lock)
        for action in fresh.actions():
            action()
        self.assertEqual((fresh.root / "scratch/staged/sample/input.h").read_text(), "content\n")
        self.assertFalse(fresh.marker.exists())

    def test_nested_initialization_copies_complete_independent_repositories_without_git(self):
        def state(source):
            # Include empty metadata directories and file modes as well as
            # every byte; filesystem identity must be independent of the seed.
            return {path.relative_to(source): (path.stat().st_mode,
                    path.read_bytes() if path.is_file() else None)
                    for path in source.rglob("*")}

        before = state(self.template_source)
        nested = self.source / "nested seed"
        child = nested / "child seed"
        for source in (nested, child):
            source.mkdir()
            with patch.object(deps.subprocess, "check_output",
                              side_effect=AssertionError("nested fixture setup started Git")):
                self.initialize(source, self.cache)
            self.assertEqual(state(source), before)
            self.assertEqual(deps.head(source, self.cache), self.template_identity["commit"])
            self.assertEqual(deps.submodules(source, self.cache), {})
        (nested / "input.h").write_text("changed nested source\n")
        (nested / ".git/config").write_text('[include]\npath = missing\n')
        (nested / ".git/HEAD").unlink()
        for source in (child, self.template_source):
            self.assertEqual(state(source), before)

    def test_nested_copy_failure_preserves_seed_and_is_cleaned_with_fixture(self):
        before = {path.relative_to(self.template_source): path.read_bytes()
                  for path in self.template_source.rglob("*") if path.is_file()}
        for failure in (PermissionError("unreadable seed"), OSError("partial nested copy")):
            with self.subTest(failure=failure):
                fresh = type(self)()
                self.addCleanup(fresh.doCleanups)
                fresh.setUp()
                source = fresh.source / "nested"
                source.mkdir()

                def fail(seed, destination, **options):
                    self.assertEqual(seed, self.template_source)
                    (destination / "partial").write_bytes(b"partial metadata")
                    raise failure

                with patch.object(shutil, "copytree", side_effect=fail):
                    with self.assertRaises(OSError) as caught:
                        fresh.initialize(source, fresh.cache)
                self.assertIs(caught.exception, failure)
                self.assertTrue((source / "partial").is_file())
                self.assertTrue(fresh.doCleanups())
                self.assertFalse(fresh.base.exists())
                self.assertEqual({path.relative_to(self.template_source): path.read_bytes()
                                  for path in self.template_source.rglob("*") if path.is_file()}, before)

    def test_failed_fixture_copy_removes_partial_data_and_preserves_template(self):
        before = {path.relative_to(self.template_source): path.read_bytes()
                  for path in self.template_source.rglob("*") if path.is_file()}
        for failure in (PermissionError("unreadable fixture"), OSError("partial fixture copy")):
            with self.subTest(failure=failure):
                fresh = type(self)()
                self.addCleanup(fresh.doCleanups)

                def fail(source, destination):
                    self.assertEqual(source, self.template_source)
                    destination.mkdir(parents=True)
                    (destination / "partial").write_bytes(b"partial copy")
                    raise failure

                with patch.object(shutil, "copytree", side_effect=fail):
                    with self.assertRaises(OSError) as caught:
                        fresh.setUp()
                self.assertIs(caught.exception, failure)
                self.assertTrue((fresh.source / "partial").is_file())
                self.assertTrue(fresh.doCleanups())
                self.assertFalse(fresh.base.exists())
                self.assertEqual({path.relative_to(self.template_source): path.read_bytes()
                                  for path in self.template_source.rglob("*") if path.is_file()}, before)

    def test_failed_template_initialization_removes_partial_repository(self):
        class FailedFixture(type(self)):
            pass

        failure = PermissionError("partial repository initialization")

        def fail(source, cache):
            self.assertEqual(source.parent, cache)
            (source / "partial").write_bytes(b"partial metadata")
            raise failure

        try:
            with patch.object(FailedFixture, "initialize", side_effect=fail):
                with self.assertRaises(PermissionError) as caught:
                    FailedFixture.setUpClass()
            self.assertIs(caught.exception, failure)
            self.assertTrue((FailedFixture.template_source / "partial").is_file())
        finally:
            FailedFixture.doClassCleanups()
        self.assertEqual(FailedFixture.tearDown_exceptions, [])
        self.assertFalse(FailedFixture.template_cache.exists())

    def test_cache_fsmonitor_is_neutralized_for_verify_stage_and_status(self):
        self.git(self.source, "config", "core.fsmonitor", self.command)
        before = self.cache_state()
        for action in self.actions():
            action()
            self.assertFalse(self.marker.exists())
        # Exercise the override independently of the plumbing-only verifier.
        self.git(self.source, "status", "--porcelain")
        self.assertFalse(self.marker.exists())
        self.assertEqual(self.cache_state(), before)

    def test_metadata_scan_checks_components_once_per_containment_and_refreshes(self):
        # Rewalking every ancestor before containment duplicates Windows
        # filesystem probes for every metadata entry, including loose objects.
        metadata = self.source / ".git"
        entries = {metadata, *metadata.rglob("*")}
        with patch.object(deps, "contained", wraps=deps.contained) as contain, \
                patch.object(deps, "reject_reparse_points", wraps=deps.reject_reparse_points) as check:
            deps.git_metadata(self.source, self.cache)
        expected = [self.source / ".git"]
        for call in contain.call_args_list:
            expected.extend(call.args)
        self.assertEqual([call.args[0] for call in check.call_args_list], expected)
        self.assertLessEqual(entries, {call.args[0] for call in contain.call_args_list})
        # A subsequent operation must inspect changed metadata afresh, even
        # when the preceding scan accepted the same repository and paths.
        config = self.source / ".git/config"
        original = config.read_bytes()
        config.write_bytes(original + b'\n[include]\npath = missing\n')
        with patch.object(deps.subprocess, "check_output", side_effect=AssertionError("Git ran")):
            self.assert_refused_without_writes()
        config.write_bytes(original)
        for action in self.actions():
            action()
        self.assertFalse(self.marker.exists())

    def test_metadata_type_lookup_failures_and_unknown_types_refuse_without_writes(self):
        entry = self.source / ".git/unknown metadata"
        entry.write_bytes(b"metadata")
        lstat = Path.lstat
        before = self.cache_state()
        lock = self.lock.read_bytes()
        for failure in (PermissionError("unreadable metadata type"),
                        OSError(errno.EIO, "partial metadata lookup"),
                        FileNotFoundError("metadata disappeared"), None):
            def lookup(path, *args, **kwargs):
                if path == entry:
                    if failure is not None:
                        raise failure
                    return SimpleNamespace(st_mode=stat.S_IFIFO | 0o600)
                return lstat(path, *args, **kwargs)

            for action in self.actions():
                expected = type(failure) if failure is not None else deps.DependencyError
                with self.subTest(failure=failure, action=action):
                    with patch.object(Path, "lstat", lookup), patch.object(
                            deps.subprocess, "check_output", side_effect=AssertionError("Git ran")):
                        with self.assertRaises(expected) as caught:
                            action()
                        if failure is not None:
                            self.assertIs(caught.exception, failure)
                        else:
                            self.assertIn("unsupported Git metadata file", str(caught.exception))
                    self.assertEqual(self.cache_state(), before)
                    self.assertEqual(self.lock.read_bytes(), lock)
                    self.assertFalse(self.marker.exists())
                    self.assertFalse((self.root / "scratch/staged").exists())
                    self.assertFalse((self.root / "scratch/init.cmake").exists())

    def test_config_includes_are_refused_in_all_metadata_configs_before_git_runs(self):
        outside = self.base / "outside config"
        outside.write_text("[core]\n\tbare = false\n")
        metadata = self.source / ".git"
        for relative in ("config", "config.worktree", "worktrees/linked/config.worktree",
                         "modules/nested/config"):
            config = metadata / relative
            config.parent.mkdir(parents=True, exist_ok=True)
            original = config.read_bytes() if config.exists() else None
            for section in ('include', 'includeIf "gitdir:never-matches"', 'INCLUDE',
                            'IncludeIf "onbranch:never-matches"', 'include.path',
                            'core] [include', 'core] [includeIf "gitdir:never-matches"',
                            'core] [fixture "a]b"] [includeIf "onbranch:never-matches"'):
                with self.subTest(config=relative, section=section):
                    config.write_bytes((original or b"") +
                                       f'\n[{section}]\n\tpath = "{outside.as_posix()}"\n'.encode())
                    with patch.object(deps.subprocess, "check_output",
                                      side_effect=AssertionError("Git ran")):
                        with self.assertRaisesRegex(deps.DependencyError, "config includes"):
                            deps.git_metadata(self.source, self.cache)
                        self.assert_refused_without_writes()
            if original is None:
                config.unlink()
            else:
                config.write_bytes(original)
        deps.verify(self.root, self.cache, self.lock)

    def test_config_include_text_in_values_and_comments_is_allowed(self):
        config = self.source / ".git/config"
        with config.open("a") as handle:
            handle.write('\n# [include] and [includeIf "gitdir:*"]\n'
                         '[fixture]\n\ttext = "[include]"\n')
        deps.verify(self.root, self.cache, self.lock)

    def test_control_whitespace_includes_refuse_every_cached_git_entry_point(self):
        outside = self.base / "outside config"
        outside.write_text(f'[core]\nfsmonitor = {self.command}\n')
        metadata = self.source / ".git"
        for relative in ("config", "config.worktree", "worktrees/linked/config.worktree",
                         "modules/nested/config"):
            config = metadata / relative
            config.parent.mkdir(parents=True, exist_ok=True)
            original = config.read_bytes() if config.exists() else b""
            for section in ('\v[include]', '\f[include]', '[core]\f[include]',
                            '\f[includeIf "gitdir:never-matches"]',
                            '[includeIf\v"gitdir:never-matches"]',
                            '[include\f]', '[includeIf "unterminated]'):
                with self.subTest(config=relative, section=section):
                    config.write_bytes(original +
                                       f'\r\n{section}\r\npath = "{outside.as_posix()}"\r\n'.encode())
                    before = outside.read_bytes()
                    with patch.object(deps.subprocess, "check_output",
                                      side_effect=AssertionError("Git ran")):
                        with self.assertRaises(deps.DependencyError):
                            deps.git_metadata(self.source, self.cache)
                        self.assert_refused_without_writes()
                        for args in (("status", "--porcelain"), ("init", "--quiet")):
                            with self.assertRaises(deps.DependencyError):
                                self.git(self.source, *args)
                    self.assertEqual(outside.read_bytes(), before)
                    self.assertFalse(self.marker.exists())
            if original:
                config.write_bytes(original)
            else:
                config.unlink()
        deps.verify(self.root, self.cache, self.lock)

    def test_real_git_external_include_control_and_guarded_refusal(self):
        outside = self.base / "outside config"
        outside.write_text('[fixture]\noutside = external-value\n'
                           f'[core]\nfsmonitor = {self.command}\n')
        config = self.source / ".git/config"
        original = config.read_bytes()
        # Git actually follows the quoted, continued path with CRLF. This
        # control uses only a temporary fixture, never an ambient config.
        path = outside.as_posix().replace("outside config", "out\\\r\nside config")
        env = {key: value for key, value in os.environ.items()
               if not key.upper().startswith("GIT_")}
        env.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
        for section in ('\r\t[include]', '[includeIf "gitdir:**"]'):
            with self.subTest(section=section):
                config.write_bytes(original +
                                   f'\r\n{section}\r\npath = "{path}"\r\n'.encode())
                result = subprocess.check_output(
                    ["git", "config", "--file", str(config), "--includes", "--get", "fixture.outside"],
                    cwd=self.source, env=env)
                self.assertEqual(result.strip(), b"external-value")
                with patch.object(deps.subprocess, "check_output",
                                  side_effect=AssertionError("Git ran")):
                    self.assert_refused_without_writes()
                    for args in (("status", "--porcelain"), ("init", "--quiet")):
                        with self.assertRaisesRegex(deps.DependencyError, "config includes"):
                            self.git(self.source, *args)
                self.assertFalse(self.marker.exists())
        config.write_bytes(original)
        self.git(self.source, "init", "--quiet")
        deps.verify(self.root, self.cache, self.lock)

    def test_unreadable_config_refuses_before_git_or_stage_writes(self):
        config = self.source / ".git/config"
        read_text = Path.read_text

        def unreadable(path, *args, **kwargs):
            if path == config:
                raise PermissionError("unreadable Git config")
            return read_text(path, *args, **kwargs)

        with patch.object(Path, "read_text", unreadable), \
                patch.object(deps.subprocess, "check_output", side_effect=AssertionError("Git ran")):
            for action in (*self.actions(),
                           lambda: self.git(self.source, "status", "--porcelain"),
                           lambda: self.git(self.source, "init", "--quiet")):
                with self.assertRaisesRegex(PermissionError, "unreadable Git config"):
                    action()
                self.assertFalse((self.root / "scratch/staged").exists())
                self.assertFalse((self.root / "scratch/init.cmake").exists())
                self.assertFalse(self.marker.exists())

    @unittest.skipIf(os.name == "nt", "POSIX metadata symlinks")
    def test_escaping_symlinked_git_config_is_refused_before_git_runs(self):
        self.git(self.source, "config", "core.fsmonitor", self.command)
        config = self.source / ".git/config"
        outside = self.base / "outside config"
        config.rename(outside)
        config.symlink_to(outside)
        with patch.object(deps.subprocess, "check_output", side_effect=AssertionError("Git ran")):
            self.assert_refused_without_writes()

    @unittest.skipIf(os.name == "nt", "POSIX metadata symlinks")
    def test_internal_symlinked_git_config_is_neutralized(self):
        self.git(self.source, "config", "core.fsmonitor", self.command)
        config = self.source / ".git/config"
        config.rename(config.with_name("local-config"))
        config.symlink_to("local-config")
        for action in self.actions():
            action()
        self.git(self.source, "status", "--porcelain")
        self.assertFalse(self.marker.exists())

    @unittest.skipIf(os.name == "nt", "POSIX metadata symlinks")
    def test_nested_and_dangling_metadata_links_are_refused(self):
        hooks = self.source / ".git/hooks"
        hooks.mkdir(exist_ok=True)
        link = hooks / "unused"
        for target in (self.base / "outside", self.base / "missing", hooks / "missing"):
            with self.subTest(target=target.name):
                if target.name == "outside":
                    target.mkdir()
                link.symlink_to(target, target_is_directory=True)
                self.assert_refused_without_writes()
                link.unlink()

    @unittest.skipIf(os.name == "nt", "POSIX metadata symlinks")
    def test_internal_directory_links_are_walked_and_cycles_are_safe(self):
        metadata = self.source / ".git"
        internal = self.cache / "linked metadata"
        internal.mkdir()
        (metadata / "linked").symlink_to(internal, target_is_directory=True)
        (internal / "cycle").symlink_to(metadata, target_is_directory=True)
        deps.git_metadata(self.source, self.cache)
        (internal / "escape").symlink_to(self.base, target_is_directory=True)
        with patch.object(deps.subprocess, "check_output", side_effect=AssertionError("Git ran")):
            self.assert_refused_without_writes()

    @unittest.skipIf(os.name == "nt", "POSIX metadata symlinks")
    def test_cyclic_metadata_symlink_is_refused(self):
        link = self.source / ".git/cyclic"
        link.symlink_to(link.name)
        self.assert_refused_without_writes()

    def test_metadata_directory_and_object_references_cannot_escape_cache(self):
        metadata = self.source / ".git"
        outside = self.base / "outside metadata"
        outside.mkdir()
        for reference in (metadata / "commondir", metadata / "objects/info/alternates"):
            with self.subTest(reference=reference.name):
                reference.parent.mkdir(exist_ok=True)
                reference.write_text(str(outside) + "\n")
                with patch.object(deps.subprocess, "check_output", side_effect=AssertionError("Git ran")):
                    self.assert_refused_without_writes()
                reference.unlink()

    def test_internal_metadata_directory_and_object_references_are_allowed(self):
        metadata = self.source / ".git"
        alternate = self.cache / "alternate objects"
        alternate.mkdir()
        (metadata / "objects/info/alternates").write_text(str(alternate) + "\n")
        deps.verify(self.root, self.cache, self.lock)
        common = metadata / "common"
        common.mkdir()
        (metadata / "commondir").write_text("common\n")
        deps.git_metadata(self.source, self.cache)

    def write_alternates(self, objects, *targets):
        info = objects / "info"
        info.mkdir(parents=True, exist_ok=True)
        # Alternate pathnames are LF-delimited even on Windows.
        (info / "alternates").write_bytes(os.fsencode("\n".join(targets) + "\n"))

    def test_alternate_fixture_uses_lf_path_delimiters(self):
        objects = self.source / ".git/objects"
        self.write_alternates(objects, "first", "second")
        self.assertEqual((objects / "info/alternates").read_bytes(), b"first\nsecond\n")

    def test_alternate_chain_supplies_real_git_objects(self):
        objects = self.source / ".git/objects"
        alternate = self.cache / "alternate objects"
        objects.rename(alternate)
        first = self.cache / "first objects"
        # Moving the objects requires Git itself to follow both references.
        # Use Git's LF framing; Windows text-writer output is checked separately.
        for parent, target in ((objects, first), (first, alternate)):
            info = parent / "info"
            info.mkdir(parents=True)
            (info / "alternates").write_bytes(os.fsencode(str(target)) + b"\n")
        before = self.cache_state()
        for action in self.actions():
            action()
        self.assertEqual(self.cache_state(), before)
        self.assertEqual((self.root / "scratch/staged/sample/input.h").read_text(), "content\n")

    def test_windows_alternate_parser_preserves_path_bytes(self):
        data = (b"# comment\r\n\r\nC:\\cache with spaces\\objects\r\n"
                b"C:/cache/objects\n../embedded\rcarriage\r\n"
                b"../trailing space \r\n../unterminated\r")
        self.assertEqual(deps.git_alternate_paths(data, windows=True),
                         ["C:\\cache with spaces\\objects", "C:/cache/objects",
                          "../embedded\rcarriage", "../trailing space ", "../unterminated\r"])
        self.assertEqual(deps.git_alternate_paths(data, windows=False),
                         ["\r", "C:\\cache with spaces\\objects\r", "C:/cache/objects",
                          "../embedded\rcarriage\r", "../trailing space \r", "../unterminated\r"])
        for windows in (False, True):
            for invalid in (b'"quoted path"\r\n', b"nul\0path\r\n"):
                with self.subTest(windows=windows, invalid=invalid):
                    with self.assertRaisesRegex(deps.DependencyError, "path encoding"):
                        deps.git_alternate_paths(invalid, windows=windows)

    def test_windows_crlf_alternate_metadata_is_contained_on_every_host(self):
        objects = self.source / ".git/objects"
        alternate = self.cache / "alternate objects"
        alternate.mkdir()
        reference = objects / "info/alternates"
        parser = deps.git_alternate_paths
        with patch.object(deps, "git_alternate_paths",
                          side_effect=lambda data, **kwargs: parser(data, windows=True)), \
                patch.object(deps.subprocess, "check_output", side_effect=AssertionError("Git ran")):
            reference.write_bytes(os.fsencode(str(alternate)) + b"\r\n")
            before = self.cache_state()
            deps.git_metadata(self.source, self.cache)
            self.assertEqual(self.cache_state(), before)
            outside = self.base / "outside objects"
            outside.mkdir()
            reference.write_bytes(os.fsencode(str(outside)) + b"\r\n")
            with self.assertRaisesRegex(deps.DependencyError, "strictly inside"):
                deps.git_metadata(self.source, self.cache)

    def test_crlf_alternate_escape_is_refused_before_git(self):
        alternate = self.base / "outside objects"
        alternate.mkdir()
        objects = self.source / ".git/objects"
        (objects / "info/alternates").write_bytes(os.fsencode(str(alternate)) + b"\r\n")
        with patch.object(deps.subprocess, "check_output", side_effect=AssertionError("Git ran")):
            with self.assertRaisesRegex(deps.DependencyError, "strictly inside"):
                deps.git_metadata(self.source, self.cache)
            self.assert_refused_without_writes()

    def test_crlf_gitdir_and_common_directory_references_remain_contained(self):
        metadata = self.source / ".git"
        saved = self.cache / "saved metadata"
        metadata.rename(saved)
        common = self.cache / "common metadata"
        common.mkdir()
        metadata.write_bytes(os.fsencode("gitdir: " + os.path.relpath(saved, metadata.parent)) + b"\r\n")
        reference = saved / "commondir"
        reference.write_bytes(os.fsencode(os.path.relpath(common, saved)) + b"\r\n")
        with patch.object(deps.subprocess, "check_output", side_effect=AssertionError("Git ran")):
            before = self.cache_state()
            deps.git_metadata(self.source, self.cache)
            self.assertEqual(self.cache_state(), before)
            outside = self.base / "outside metadata"
            outside.mkdir()
            reference.write_bytes(os.fsencode(str(outside)) + b"\r\n")
            with self.assertRaisesRegex(deps.DependencyError, "strictly inside"):
                deps.git_metadata(self.source, self.cache)

    def test_unicode_separators_are_preserved_in_contained_alternate_paths(self):
        objects = self.source / ".git/objects"
        before = self.lock.read_bytes()
        for separator in ("\x85", "\u2028", "\u2029"):
            with self.subTest(separator=separator):
                alternate = self.cache / f"alternate{separator}objects"
                alternate.mkdir()
                self.write_alternates(objects, str(alternate))
                state = self.cache_state()
                deps.verify(self.root, self.cache, self.lock)
                self.assertEqual(self.cache_state(), state)
                self.assertEqual(self.lock.read_bytes(), before)

    def test_alternate_path_separators_cannot_split_containment_checks(self):
        objects = self.source / ".git/objects"
        (self.cache / "x").mkdir()
        (self.cache / "outside").mkdir()
        # Each fragment alone is inside the cache; the actual pathname escapes.
        # Refuse before pathname lookup so Windows also exercises these bytes.
        for separator in (b"\v", b"\f", b"\r", b"\x1c", b"\x1d", b"\x1e",
                          b"\xc2\x85", b"\xe2\x80\xa8", b"\xe2\x80\xa9"):
            with self.subTest(separator=separator):
                path = os.fsencode(self.cache) + b"/x" + separator + b"../../../outside\n"
                (objects / "info/alternates").write_bytes(path)
                with patch.object(deps.subprocess, "check_output", side_effect=AssertionError("Git ran")):
                    with self.assertRaisesRegex(deps.DependencyError, "strictly inside"):
                        deps.git_metadata(self.source, self.cache)
                    self.assert_refused_without_writes()

    def test_lexical_containment_refuses_escapes_before_filesystem_lookup(self):
        # Exercise the Windows refusal order on every host, including bytes
        # that cannot be passed to a Windows filesystem API.
        for name in ("..", "../outside", "x\v../../../outside", "x\r../../../outside", "."):
            with self.subTest(name=name), patch.object(
                    deps, "reject_reparse_points", side_effect=AssertionError("filesystem lookup ran")):
                with self.assertRaisesRegex(deps.DependencyError, "strictly inside"):
                    deps.contained(self.cache / name, self.cache)

    def test_resolved_aliases_require_containment_and_original_path_checks(self):
        # Model a Windows short name without requiring 8.3 generation or
        # symlink privileges. Resolving may expand a path outside its spelling.
        alias = self.base / "CACHE~1/objects"
        resolve = Path.resolve
        for target in (self.cache / "objects", self.base / "outside", self.cache):
            with self.subTest(target=target), patch.object(
                    Path, "resolve", lambda path: target if path == alias else resolve(path)), \
                    patch.object(deps, "reject_reparse_points") as check:
                if target == self.cache / "objects":
                    self.assertEqual(deps.contained(alias, self.cache), target)
                else:
                    with self.assertRaisesRegex(deps.DependencyError, "strictly inside"):
                        deps.contained(alias, self.cache)
                self.assertEqual([call.args[0] for call in check.call_args_list], [alias, self.cache])
        for failure in (PermissionError("unreadable alias"), RuntimeError("cyclic alias")):
            def failed_resolve(path):
                if path == alias:
                    raise failure
                return resolve(path)
            with self.subTest(failure=type(failure)), patch.object(Path, "resolve", failed_resolve), \
                    patch.object(deps, "reject_reparse_points"):
                expected = PermissionError if isinstance(failure, PermissionError) else deps.DependencyError
                message = "unreadable alias" if isinstance(failure, PermissionError) else "cyclic path"
                with self.assertRaisesRegex(expected, message):
                    deps.contained(alias, self.cache)

    def test_head_uses_commit_peeling_without_argument_globs(self):
        expected = json.loads(self.lock.read_text())["dependencies"]["sample"]["commit"]
        for ending in (b"\n", b"\r\n"):
            with self.subTest(ending=ending), patch.object(
                    deps.subprocess, "check_output", return_value=expected.encode("ascii") + ending) as run:
                self.assertEqual(deps.head(self.source, self.cache), expected)
                self.assertEqual(run.call_args.args[0][-3:], ["rev-parse", "--verify", "HEAD^0"])

    def test_common_directory_alternates_cannot_escape_cache(self):
        metadata = self.source / ".git"
        common = self.cache / "common metadata"
        outside = self.base / "outside objects"
        outside.mkdir()
        # Build complete common metadata before installing the hostile
        # alternate. Fixture construction must not invoke the guarded Git
        # wrapper after introducing the escape it is intended to refuse.
        shutil.copytree(metadata, common)
        self.write_alternates(common / "objects", str(outside))
        (metadata / "commondir").write_text(os.path.relpath(common, metadata) + "\n")
        with patch.object(deps.subprocess, "check_output", side_effect=AssertionError("Git ran")):
            with self.assertRaisesRegex(deps.DependencyError, "strictly inside"):
                deps.git_metadata(self.source, self.cache)
            self.assert_refused_without_writes()

    def test_chained_relative_alternates_cannot_escape_cache(self):
        objects = self.source / ".git/objects"
        first = self.cache / "first objects"
        second = self.cache / "second objects"
        outside = self.base / "outside objects"
        outside.mkdir()
        self.write_alternates(objects, os.path.relpath(first, objects))
        self.write_alternates(first, "# ignored comment", "", os.path.relpath(second, first))
        self.write_alternates(second, os.path.relpath(outside, second))
        with patch.object(deps.subprocess, "check_output", side_effect=AssertionError("Git ran")):
            self.assert_refused_without_writes()

    def test_alternate_and_common_directory_cycles_are_refused_before_git(self):
        metadata = self.source / ".git"
        objects = metadata / "objects"
        alternate = self.cache / "alternate objects"
        self.write_alternates(objects, str(alternate))
        self.write_alternates(alternate, os.path.relpath(objects, alternate))
        with patch.object(deps.subprocess, "check_output", side_effect=AssertionError("Git ran")):
            with self.assertRaisesRegex(deps.DependencyError, "cyclic"):
                deps.git_metadata(self.source, self.cache)
            self.assert_refused_without_writes()
        (objects / "info/alternates").unlink()
        common = self.cache / "common metadata"
        common.mkdir()
        (metadata / "commondir").write_text(str(common) + "\n")
        (common / "commondir").write_text(str(metadata) + "\n")
        with patch.object(deps.subprocess, "check_output", side_effect=AssertionError("Git ran")):
            with self.assertRaisesRegex(deps.DependencyError, "cyclic"):
                deps.git_metadata(self.source, self.cache)
            self.assert_refused_without_writes()

    def test_alternate_redirection_depth_is_bounded_before_git(self):
        previous = self.source / ".git/objects"
        for index in range(deps.MAX_GIT_REDIRECTIONS + 1):
            objects = self.cache / f"objects-{index}"
            objects.mkdir()
            self.write_alternates(previous, os.path.relpath(objects, previous))
            previous = objects
        with patch.object(deps.subprocess, "check_output", side_effect=AssertionError("Git ran")):
            with self.assertRaisesRegex(deps.DependencyError, "depth exceeded"):
                deps.git_metadata(self.source, self.cache)
            self.assert_refused_without_writes()

    def test_reused_alternate_store_is_accepted_without_mistaking_it_for_a_cycle(self):
        objects = self.source / ".git/objects"
        first, second, shared = (self.cache / name for name in ("first", "second", "shared"))
        shared.mkdir()
        self.write_alternates(objects, str(first), str(second))
        self.write_alternates(first, os.path.relpath(shared, first))
        self.write_alternates(second, os.path.relpath(shared, second))
        before = self.cache_state()
        deps.verify(self.root, self.cache, self.lock)
        self.assertEqual(self.cache_state(), before)

    def test_deeper_reuse_of_a_checked_subtree_still_enforces_depth_bound(self):
        objects = self.source / ".git/objects"
        shared, tail = self.cache / "shared", self.cache / "tail"
        tail.mkdir()
        self.write_alternates(shared, str(tail))
        chain = [self.cache / f"deep-{index}" for index in range(deps.MAX_GIT_REDIRECTIONS - 2)]
        self.write_alternates(objects, str(shared), str(chain[0]))
        for parent, child in zip(chain, [*chain[1:], shared]):
            self.write_alternates(parent, str(child))
        with patch.object(deps.subprocess, "check_output", side_effect=AssertionError("Git ran")):
            with self.assertRaisesRegex(deps.DependencyError, "depth exceeded"):
                deps.git_metadata(self.source, self.cache)
            self.assert_refused_without_writes()

    def test_malformed_common_and_encoded_alternate_paths_are_refused(self):
        metadata = self.source / ".git"
        for text in ("", "\n", "first\nsecond\n", "\0"):
            with self.subTest(common=text):
                (metadata / "commondir").write_text(text)
                with patch.object(deps.subprocess, "check_output", side_effect=AssertionError("Git ran")):
                    with self.assertRaisesRegex(deps.DependencyError, "invalid Git common directory"):
                        deps.git_metadata(self.source, self.cache)
                    self.assert_refused_without_writes()
        (metadata / "commondir").unlink()
        for text in ('"quoted path"', "\0"):
            with self.subTest(alternate=text):
                self.write_alternates(metadata / "objects", text)
                with patch.object(deps.subprocess, "check_output", side_effect=AssertionError("Git ran")):
                    with self.assertRaisesRegex(deps.DependencyError, "path encoding"):
                        deps.git_metadata(self.source, self.cache)
                    self.assert_refused_without_writes()

    def test_in_root_common_and_alternate_chain_supplies_real_git_objects(self):
        metadata = self.source / ".git"
        common = self.cache / "common metadata"
        metadata.rename(common)
        metadata.mkdir()
        (metadata / "HEAD").write_bytes((common / "HEAD").read_bytes())
        (metadata / "commondir").write_text(os.path.relpath(common, metadata) + "\n")
        first = self.cache / "first objects"
        second = self.cache / "second objects"
        (common / "objects").rename(second)
        self.write_alternates(common / "objects", "# comment", "", os.path.relpath(first, common / "objects"))
        self.write_alternates(first, os.path.relpath(second, first))
        before = self.cache_state()
        for action in self.actions():
            action()
        self.assertEqual(self.cache_state(), before)
        self.assertEqual((self.root / "scratch/staged/sample/input.h").read_text(), "content\n")

    def test_gitdir_file_chains_are_contained_and_cycles_are_refused(self):
        metadata = self.source / ".git"
        saved = self.cache / "saved metadata"
        metadata.rename(saved)
        redirect = self.cache / "gitdir redirect"
        metadata.write_text("gitdir: " + os.path.relpath(redirect, metadata.parent) + "\n")
        redirect.write_text("gitdir: " + os.path.relpath(saved, redirect.parent) + "\n")
        deps.git_metadata(self.source, self.cache)
        for target, reason in ((metadata, "cyclic"), (self.base / "outside", "strictly inside")):
            with self.subTest(reason=reason):
                redirect.write_text("gitdir: " + str(target) + "\n")
                before = self.cache_state()
                with patch.object(deps.subprocess, "check_output", side_effect=AssertionError("Git ran")):
                    with self.assertRaisesRegex(deps.DependencyError, reason):
                        deps.git(self.source, "rev-parse", "HEAD", cache=self.cache)
                self.assertEqual(self.cache_state(), before)

    def test_git_configuration_environment_and_protocols_are_isolated(self):
        injected = {"GIT_CONFIG_COUNT": "1", "GIT_CONFIG_KEY_0": "core.fsmonitor",
                    "GIT_CONFIG_VALUE_0": self.command, "GIT_DIR": str(self.base / "wrong"),
                    "GIT_CONFIG_PARAMETERS": "'core.fsmonitor'='injected'",
                    "GIT_CONFIG_SYSTEM": str(self.base / "system"),
                    "GIT_SSH_COMMAND": self.command, "GIT_ASKPASS": self.command,
                    "GIT_TRACE": str(self.marker), "GIT_ALLOW_PROTOCOL": "ext:file:ssh:https",
                    "GIT_COMMON_DIR": str(self.base / "outside common"),
                    "GIT_OBJECT_DIRECTORY": str(self.base / "outside objects"),
                    "GIT_ALTERNATE_OBJECT_DIRECTORIES": str(self.base / "outside alternates"),
                    "GIT_WORK_TREE": str(self.base / "outside worktree")}
        for network in (False, True):
            with self.subTest(network=network), patch.dict(os.environ, injected):
                with patch.object(deps.subprocess, "check_output", return_value=b"path \0") as run:
                    self.assertEqual(deps.git(self.source, "ls-tree", "-rz", "HEAD",
                                              cache=self.cache, network=network), "path \0")
                env = run.call_args.kwargs["env"]
                for key in injected.keys() - {"GIT_ALLOW_PROTOCOL"}:
                    self.assertNotIn(key, env)
                self.assertEqual(env["GIT_CONFIG_NOSYSTEM"], "1")
                self.assertEqual(env["GIT_CONFIG_GLOBAL"], os.devnull)
                self.assertEqual(env["GIT_ALLOW_PROTOCOL"], "https" if network else "")
                command = run.call_args.args[0]
                for value in ("core.fsmonitor=false", f"core.hooksPath={os.devnull}",
                              f"core.sshCommand={os.devnull}", f"core.askPass={os.devnull}",
                              f"core.attributesFile={os.devnull}", f"core.excludesFile={os.devnull}",
                              "credential.helper=", "credential.interactive=false",
                              "protocol.allow=never", "submodule.recurse=false"):
                    index = command.index(value)
                    self.assertEqual(command[index - 1], "-c")
        # Real Git must also ignore an injected repository and monitor config.
        with patch.dict(os.environ, injected):
            deps.verify(self.root, self.cache, self.lock)
            self.git(self.source, "status", "--porcelain")
        self.assertFalse(self.marker.exists())

    def test_cache_transport_config_cannot_enable_external_or_file_helpers(self):
        for protocol, url in (("ext", "ext::" + self.command), ("file", str(self.source))):
            with self.subTest(protocol=protocol):
                # Write the hostile fixture directly; only the operation
                # under test should pass it through the guarded Git wrapper.
                with (self.source / ".git/config").open("a", encoding="utf-8") as config:
                    config.write(f'\n[protocol "{protocol}"]\n\tallow = always\n')
                with self.assertRaises(subprocess.CalledProcessError) as refused:
                    self.git(self.source, "ls-remote", url)
                self.assertIn(f"transport '{protocol}' not allowed".encode(), refused.exception.stderr)
                self.assertFalse(self.marker.exists())

    def test_real_recursive_gitlinks_check_initialized_heads(self):
        nested = self.source / "nested path (v1)"
        nested.mkdir()
        self.initialize(nested, self.cache)
        child = nested / "child"
        child.mkdir()
        self.initialize(child, self.cache)
        child_head = deps.head(child, self.cache)
        self.git(nested, "update-index", "--add", "--cacheinfo", "160000", child_head, "child")
        self.commit(nested, self.cache)
        nested_head = deps.head(nested, self.cache)
        self.git(self.source, "update-index", "--add", "--cacheinfo", "160000", nested_head, nested.name)
        self.commit(self.source, self.cache)
        # Initialized submodules normally have a gitdir file, not a directory.
        metadata = self.source / ".git/modules/nested"
        metadata.parent.mkdir()
        (nested / ".git").rename(metadata)
        (nested / ".git").write_text("gitdir: ../.git/modules/nested\n")
        self.write_lock()
        self.assertEqual(deps.submodules(self.source, self.cache),
                         {nested.name: nested_head, nested.name + "/child": child_head})
        deps.verify(self.root, self.cache, self.lock)
        self.git(child, "-c", "user.name=Fixture", "-c", "user.email=fixture@example.org",
                 "-c", "commit.gpgSign=false", "-c", "maintenance.auto=false",
                 "commit", "--quiet", "--allow-empty", "-m", "changed")
        # Keep the content digest correct while changing only submodule HEAD.
        self.assert_refused_without_writes()
        (child / ".git/HEAD").unlink()
        self.assert_refused_without_writes()

    def test_submodule_gitdir_outside_cache_is_refused(self):
        nested = self.source / "nested"
        nested.mkdir()
        self.initialize(nested, self.cache)
        nested_head = deps.head(nested, self.cache)
        self.git(self.source, "update-index", "--add", "--cacheinfo", "160000", nested_head, "nested")
        self.commit(self.source, self.cache)
        self.write_lock()
        metadata = self.base / "outside metadata"
        (nested / ".git").rename(metadata)
        (nested / ".git").write_text("gitdir: " + str(metadata) + "\n")
        self.assert_refused_without_writes()


class ScratchPreparationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="scratch preparation ")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name).resolve()
        self.root = self.base / "nested checkout" / "clone with spaces"
        self.root.mkdir(parents=True)
        self.outside = self.base / "outside"
        self.outside.mkdir()
        (self.outside / "retained bytes").write_bytes(b"unchanged\x00\xff\n")

    def state(self):
        def visit(path):
            info = path.lstat()
            if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
                return ("redirect", os.readlink(path))
            if stat.S_ISDIR(info.st_mode):
                return {child.name: visit(child) for child in path.iterdir()}
            if stat.S_ISREG(info.st_mode):
                return path.read_bytes()
            return ("special", info.st_mode)
        return visit(self.base)

    def preparation(self, root=None, child=False):
        # Execute the documented command through its real CLI parser. Only
        # nice and the interpreter launch are elided, so this also runs on
        # Windows without a shell, compiler, CMake or a dependency snapshot.
        docs = (ROOT / "docs/ci/local-checks.md").read_text(encoding="utf-8")
        recipe = docs.split("## Worker recipe\n", 1)[1].split("```bash\n", 1)[1].split("\n```", 1)[0]
        prefix, separator, _ = recipe.partition("export TMPDIR=")
        self.assertTrue(separator)
        lines = [line for line in prefix.replace("\\\n", "").splitlines() if line.strip()]
        self.assertEqual(len(lines), 4, "preparation must precede mkdir and environment exports")
        argv = shlex.split(lines[-1])
        self.assertEqual(argv[:7], ["nice", "-n", "10", "python3", "-I", "-B", "tools/ci/offline_deps.py"])
        argv = [value.replace("$PWD", str(root or self.root)).replace(
            "$LUMINUMBRA_DEPS_CACHE", str(self.outside)) for value in argv[7:]]
        if child:
            result = subprocess.run([sys.executable, "-I", "-B", str(ROOT / "tools/ci/offline_deps.py"), *argv],
                                    capture_output=True, text=True, check=False)
            self.assertNotIn("Traceback", result.stderr)
            if result.returncode == 1:
                self.assertEqual(result.stdout, "")
                self.assertTrue(result.stderr.startswith("offline dependencies: "))
            return result.returncode
        output = io.StringIO()
        with patch.object(sys, "stdout", output), redirect_stderr(io.StringIO()) as diagnostic:
            result = deps.main(argv)
        self.assertNotIn("Traceback", diagnostic.getvalue())
        if result == 1:
            self.assertEqual(output.getvalue(), "")
            self.assertTrue(diagnostic.getvalue().startswith("offline dependencies: "))
        return result

    def refused(self, root=None):
        before = self.state()
        self.assertEqual(self.preparation(root), 1)
        self.assertEqual(self.state(), before)

    def redirect(self, path, target):
        if os.name == "nt":
            # Junction creation needs no symlink privilege. These fixtures
            # are required: setup failure must fail the refusal regression.
            result = subprocess.run(["cmd", "/d", "/c", "mklink", "/J", str(path), str(target)],
                                    capture_output=True, text=True, check=False)
            if result.returncode:
                diagnostic = (f"junction creation failed: status={result.returncode}; "
                              f"stdout={result.stdout!r}; stderr={result.stderr!r}")
                self.assertFalse(path.exists(), diagnostic)
                self.fail(diagnostic)
        else:
            path.symlink_to(target, target_is_directory=True)
        self.addCleanup(self.remove_redirect, path)

    def remove_redirect(self, path):
        try:
            info = path.lstat()
        except FileNotFoundError:
            return
        if stat.S_ISLNK(info.st_mode):
            path.unlink()
        elif getattr(info, "st_file_attributes", 0) & 0x400:
            os.rmdir(path)

    def test_required_windows_junction_setup_failure_is_an_assertion_with_captured_diagnostics(self):
        path = self.root / "required junction"
        before = self.state()
        for status, stdout, stderr in ((1, "partial output\r\n", "access denied\r\n"),
                                       (5, "", ""), (-1, "partial", "incomplete error")):
            with self.subTest(status=status):
                result = subprocess.CompletedProcess([], status, stdout, stderr)
                with patch.object(os, "name", "nt"), \
                        patch.object(subprocess, "run", return_value=result) as run:
                    with self.assertRaises(AssertionError) as failure:
                        self.redirect(path, self.outside)
                run.assert_called_once_with(
                    ["cmd", "/d", "/c", "mklink", "/J", str(path), str(self.outside)],
                    capture_output=True, text=True, check=False)
                for captured in (f"status={status}", f"stdout={stdout!r}", f"stderr={stderr!r}"):
                    self.assertIn(captured, str(failure.exception))
                self.assertEqual(self.state(), before)

    def test_successful_windows_junction_setup_reaches_every_real_refusal_caller(self):
        # Model only the Windows command on POSIX; every caller still uses
        # real redirects and the real preparation/refusal assertions below.
        windows = os.name == "nt"
        run = subprocess.run
        cases = (
            ("test_documented_preparation_allows_nested_ordinary_trees_and_rechecks_each_call", 1, 1),
            ("test_documented_preparation_cli_checks_real_paths_in_a_child_process", 1, 0),
            ("test_documented_preparation_refuses_real_root_child_tmp_cache_and_build_redirects", 12, 12),
            ("test_documented_preparation_refuses_original_components_before_dotdot", 1, 1 if windows else 5),
        )
        for name, redirects, refusals in cases:
            with self.subTest(caller=name):
                case = ScratchPreparationTests(name)
                redirect = case.redirect

                def required(path, target):
                    def create(command, **kwargs):
                        if windows:
                            return run(command, **kwargs)
                        path.symlink_to(target, target_is_directory=True)
                        return subprocess.CompletedProcess(command, 0, "junction created\r\n", "")

                    with patch.object(os, "name", "nt"), patch.object(subprocess, "run", side_effect=create):
                        redirect(path, target)

                result = unittest.TestResult()
                with patch.object(case, "redirect", side_effect=required) as setup, \
                        patch.object(case, "refused", wraps=case.refused) as refused, \
                        patch.object(case, "preparation", wraps=case.preparation) as preparation:
                    case.run(result)
                self.assertEqual(result.testsRun, 1)
                self.assertEqual(result.failures, [])
                self.assertEqual(result.errors, [])
                self.assertEqual(result.skipped, [])
                self.assertEqual(setup.call_count, redirects)
                self.assertEqual(refused.call_count, refusals)
                if "child_process" in name:
                    self.assertEqual(preparation.call_args_list,
                                     [unittest.mock.call(child=True), unittest.mock.call(child=True)])

    def test_documented_preparation_allows_nested_ordinary_trees_and_rechecks_each_call(self):
        for name in ("tmp", "ccache", "local-gcc", "local-tidy"):
            nested = self.root / "scratch" / name / "ordinary nested" / "deeper"
            nested.mkdir(parents=True)
            (nested / "retained file").write_bytes(b"ordinary\n")
        before = self.state()
        for _ in range(2):
            self.assertEqual(self.preparation(), 0)
            self.assertEqual(self.state(), before)
        ordinary = self.root / "ordinary"
        ordinary.mkdir()
        self.assertEqual(self.preparation(ordinary / ".."), 0)
        redirected = self.root / "scratch/local-tidy/new redirect"
        self.redirect(redirected, self.outside)
        self.refused()

    def test_documented_preparation_creates_only_tmp_cache_after_all_checks(self):
        self.assertEqual(self.preparation(), 0)
        self.assertEqual({path.name for path in (self.root / "scratch").iterdir()}, {"tmp", "ccache"})
        self.assertEqual(list((self.root / "scratch/tmp").iterdir()), [])
        self.assertEqual(list((self.root / "scratch/ccache").iterdir()), [])
        for relative in ("offline-deps", "offline-init.cmake"):
            path = self.root / "scratch" / relative
            path.write_bytes(b"retained stage occupant")
            self.refused()
            path.unlink()

    def test_documented_preparation_cli_checks_real_paths_in_a_child_process(self):
        self.assertEqual(self.preparation(child=True), 0)
        tmp = self.root / "scratch/tmp"
        tmp.rmdir()
        self.redirect(tmp, self.outside)
        before = self.state()
        self.assertEqual(self.preparation(child=True), 1)
        self.assertEqual(self.state(), before)

    def test_documented_preparation_refuses_real_root_child_tmp_cache_and_build_redirects(self):
        for relative in ("scratch", "scratch/tmp", "scratch/ccache", "scratch/local-gcc",
                         "scratch/local-tidy", "scratch/tmp" "/nested", "scratch/ccache/nested",
                         "scratch/local-gcc/_deps", "scratch/local-tidy/CMakeFiles",
                         "scratch/offline-deps", "scratch/offline-init.cmake"):
            with self.subTest(relative=relative):
                link = self.root / relative
                link.parent.mkdir(parents=True, exist_ok=True)
                self.redirect(link, self.outside)
                self.refused()
                if os.name == "nt":
                    os.rmdir(link)
                else:
                    link.unlink()
        alias = self.base / "checkout alias"
        self.redirect(alias, self.outside)
        self.refused(alias)

    def test_documented_preparation_refuses_original_components_before_dotdot(self):
        alias = self.root / "redirect"
        self.redirect(alias, self.outside)
        self.refused(alias / ".." / self.root.name)
        if os.name == "posix":
            scratch = self.root / "scratch"
            scratch.mkdir(exist_ok=True)
            for target in ("missing", "tmp", "."):
                with self.subTest(target=target):
                    link = scratch / "tmp"
                    link.symlink_to(target, target_is_directory=True)
                    self.refused()
                    link.unlink()
            # A generated file can redirect writes just like a directory.
            build = scratch / "local-gcc"
            build.mkdir()
            (build / "compile_commands.json").symlink_to(self.outside / "retained bytes")
            self.refused()

    def test_documented_preparation_refuses_non_directory_components_and_unknown_entry_types(self):
        for relative in ("scratch", "scratch/tmp", "scratch/ccache", "scratch/local-gcc",
                         "scratch/local-tidy"):
            with self.subTest(relative=relative):
                path = self.root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"not a directory")
                self.refused()
                path.unlink()
        entry = self.root / "scratch/local-tidy/unknown"
        entry.parent.mkdir()
        entry.write_bytes(b"placeholder")
        lstat = Path.lstat
        with patch.object(Path, "lstat", lambda path: SimpleNamespace(st_mode=0) if path == entry else lstat(path)):
            self.refused()
        if os.name == "posix":
            entry.unlink()
            os.mkfifo(entry)
            self.refused()

    def test_documented_preparation_refuses_windows_reparse_components_without_privilege(self):
        scratch = self.root / "scratch"
        directories = [scratch / name for name in ("tmp", "ccache", "local-gcc", "local-tidy")]
        for path in directories:
            path.mkdir(parents=True)
        entry = scratch / "local-tidy/nested/generated"
        entry.parent.mkdir(parents=True)
        entry.write_bytes(b"retained generated input")
        lstat = os.lstat
        for marked in (self.root, scratch, *directories, entry.parent, entry):
            def metadata(path, *args, **kwargs):
                info = lstat(path, *args, **kwargs)
                fields = {name: getattr(info, name) for name in dir(info) if name.startswith("st_")}
                fields.update(st_file_attributes=0x400 if Path(path) == marked else 0,
                              st_reparse_tag=0xDEADBEEF)
                return SimpleNamespace(**fields)
            before = self.state()
            with self.subTest(marked=marked), patch.object(deps._PATH_SAFETY, "WINDOWS", True), \
                    patch.object(os, "lstat", side_effect=metadata):
                self.assertEqual(self.preparation(), 1)
            self.assertEqual(self.state(), before)

    def test_documented_preparation_rechecks_resolved_containment_for_every_output(self):
        resolve = Path.resolve
        for relative in ("scratch", "scratch/tmp", "scratch/ccache", "scratch/local-gcc",
                         "scratch/local-tidy", "scratch/offline-deps", "scratch/offline-init.cmake"):
            target = self.root / relative
            with self.subTest(relative=relative), patch.object(
                    Path, "resolve", lambda path: self.outside if path == target else resolve(path)):
                self.refused()

    def test_documented_preparation_refuses_lookup_resolution_and_partial_scan_errors_without_writes(self):
        entry = self.root / "scratch/local-tidy/nested/retained"
        entry.parent.mkdir(parents=True)
        entry.write_bytes(b"same bytes")
        lstat, scandir = Path.lstat, os.scandir
        for failure in (PermissionError(errno.EACCES, "lookup denied"), OSError(errno.EIO, "partial lookup"),
                        OSError(errno.ELOOP, "cyclic lookup"), RuntimeError("cyclic lookup")):
            with self.subTest(failure=failure), patch.object(Path, "resolve", side_effect=failure):
                self.refused()
        for method, original in (("lstat", lstat), ("scandir", scandir)):
            for failure in (PermissionError(errno.EACCES, "scan denied"), OSError(errno.EIO, "partial scan")):
                def failed(path, *args, **kwargs):
                    if Path(path) == entry.parent:
                        raise failure
                    return original(path, *args, **kwargs)
                before = self.state()
                owner = os if method == "scandir" else Path
                with self.subTest(method=method, failure=failure), patch.object(owner, method, failed):
                    self.assertEqual(self.preparation(), 1)
                self.assertEqual(self.state(), before)
        # Failure after a real directory entry was returned must also precede
        # tmp/cache creation, and the scanner must still be closed.
        class PartialScan:
            def __enter__(scan):
                scan.entries = scandir(entry.parent)
                scan.first = True
                return scan

            def __exit__(scan, *args):
                scan.entries.close()

            def __iter__(scan):
                return scan

            def __next__(scan):
                if scan.first:
                    scan.first = False
                    return next(scan.entries)
                raise OSError(errno.EIO, "partial directory scan")

        before = self.state()
        with patch.object(os, "scandir", lambda path: PartialScan() if Path(path) == entry.parent else scandir(path)):
            self.assertEqual(self.preparation(), 1)
        self.assertEqual(self.state(), before)
        mkdir = Path.mkdir
        for failure in (PermissionError(errno.EACCES, "creation denied"), OSError(errno.EIO, "creation failed")):
            def failed_mkdir(path, *args, **kwargs):
                if path == self.root / "scratch/tmp":
                    raise failure
                return mkdir(path, *args, **kwargs)
            with self.subTest(failure=failure), patch.object(Path, "mkdir", failed_mkdir):
                self.refused()
        # An I/O error creating the second directory may leave the first
        # empty directory; no cleanup may remove a pre-existing occupant.
        def failed_cache_mkdir(path, *args, **kwargs):
            if path == self.root / "scratch/ccache":
                raise PermissionError(errno.EACCES, "cache creation denied")
            return mkdir(path, *args, **kwargs)
        before = self.state()
        with patch.object(Path, "mkdir", failed_cache_mkdir):
            self.assertEqual(self.preparation(), 1)
        self.assertEqual(list((self.root / "scratch/tmp").iterdir()), [])
        self.assertFalse((self.root / "scratch/ccache").exists())
        (self.root / "scratch/tmp").rmdir()
        self.assertEqual(self.state(), before)
        self.refused(self.base / "missing checkout")
        self.refused(self.outside / "retained bytes")


class WindowsReparseMetadataTests(unittest.TestCase):
    def test_attributes_junction_api_and_unknown_tags(self):
        safety = deps._PATH_SAFETY
        for attributes, expected in ((0, False), (0x400, True), (0x401, True)):
            with self.subTest(attributes=attributes), \
                    patch.object(safety.os, "lstat", return_value=SimpleNamespace(
                        st_file_attributes=attributes, st_reparse_tag=0xDEADBEEF)):
                self.assertEqual(safety.is_reparse_point("entry"), expected)
        with patch.object(safety.os, "lstat", return_value=SimpleNamespace()), \
                patch.object(safety.os.path, "isjunction", return_value=True, create=True):
            self.assertTrue(safety.is_reparse_point("entry"))

    def test_legacy_attribute_api_and_failure_inputs(self):
        safety = deps._PATH_SAFETY
        kernel = MagicMock()
        with patch.object(safety, "WINDOWS", True), \
                patch.object(safety.os, "lstat", return_value=SimpleNamespace()), \
                patch.object(safety.os.path, "isjunction", return_value=False, create=True), \
                patch.object(ctypes, "WinDLL", return_value=kernel, create=True), \
                patch.object(ctypes, "get_last_error", return_value=5, create=True), \
                patch.object(ctypes, "WinError", side_effect=PermissionError("attributes denied"), create=True):
            for attributes, expected in ((0, False), (0x400, True)):
                kernel.GetFileAttributesW.return_value = attributes
                self.assertEqual(safety.is_reparse_point("entry"), expected)
            kernel.GetFileAttributesW.return_value = 0xFFFFFFFF
            with self.assertRaisesRegex(PermissionError, "attributes denied"):
                safety.is_reparse_point("entry")
        for error in (FileNotFoundError(), PermissionError("unreadable")):
            with self.subTest(error=type(error)), patch.object(safety.os, "lstat", side_effect=error):
                if isinstance(error, FileNotFoundError):
                    self.assertFalse(safety.is_reparse_point("entry"))
                else:
                    with self.assertRaises(PermissionError):
                        safety.is_reparse_point("entry")

    def test_original_components_are_checked_before_dotdot_resolution(self):
        safety = deps._PATH_SAFETY
        with patch.object(safety, "WINDOWS", True), \
                patch.object(safety, "is_reparse_point", side_effect=lambda p: p == "redirect"):
            with self.assertRaisesRegex(OSError, "reparse point"):
                safety.reject_reparse_points(os.path.join("redirect", "..", "ordinary", "new"))
            safety.reject_reparse_points("")
        with patch.object(safety, "WINDOWS", False), patch.object(safety, "is_reparse_point") as probe:
            safety.reject_reparse_points("entry")
            probe.assert_not_called()




class _ElapsedSample:
    MAX = (1 << 63) - 1

    def __init__(self, clock):
        self.clock = clock
        self.valid = True
        # G then S: started, closed, raised, inclusive_ns, exclusive_ns.
        self.counters = [0] * 10
        # Only the active lifecycle owns these 24 frame cells and 10 counters.
        self.frames = [[0, 0, 0] for _ in range(8)]
        self.depth = 0
        self.begin = None
        self.life = None
        self.last = None
        # Two preallocated wrapper-local monotonic coverage-fault latches.
        self.fault_latches = ([False], [False])

    def now(self):
        value = self.clock()
        if (type(value) is not int or not 0 <= value <= self.MAX
                or (self.last is not None and value < self.last)):
            raise ValueError("invalid monotonic sample")
        self.last = value
        return value

    def start(self):
        try:
            self.begin = self.now()
        except BaseException:
            self.valid = False

    def enter(self, category):
        if not self.valid:
            return None
        if self.depth == 8 or self.counters[0] + self.counters[5] == 4096:
            self.valid = False
            return None
        try:
            stamp = self.now()
            frame = self.frames[self.depth]
            frame[:] = category, stamp, 0
            self.depth += 1
            self.counters[category] += 1
            return self.depth - 1
        except BaseException:
            self.valid = False
            return None

    def leave(self, token, returned):
        if token is None:
            return
        try:
            if self.depth != token + 1:
                raise ValueError("unbalanced frames")
            category, stamp, child = self.frames[token]
            if self.valid:
                elapsed = self.now() - stamp
                exclusive = elapsed - child
                if not 0 <= exclusive <= elapsed <= self.MAX:
                    raise ValueError("invalid elapsed interval")
                values = (self.counters[category + 1] + 1,
                          self.counters[category + 2] + (not returned),
                          self.counters[category + 3] + elapsed,
                          self.counters[category + 4] + exclusive)
                if any(value > self.MAX for value in values):
                    raise ValueError("counter overflow")
                self.counters[category + 1:category + 5] = values
                if token:
                    total = self.frames[token - 1][2] + elapsed
                    if total > self.MAX:
                        raise ValueError("frame overflow")
                    self.frames[token - 1][2] = total
        except BaseException:
            self.valid = False
        finally:
            self.depth = token

    def observe_faults(self):
        if self.fault_latches[0][0] or self.fault_latches[1][0]:
            self.valid = False

    def stop(self):
        try:
            self.observe_faults()
            if self.begin is None or self.depth:
                raise ValueError("missing endpoint")
            self.life = self.now() - self.begin
            union = self.counters[4] + self.counters[9]
            if not 0 <= union <= self.life <= self.MAX:
                raise ValueError("invalid residual")
        except BaseException:
            self.valid = False

    def wrap(self, target, category, owner_thread, get_ident):
        sample = self
        # Bind the two fixed latch references before either slot is installed.
        # A synthetic foreign-only delegate need not provide sample storage.
        try:
            fault = self.fault_latches[category // 5]
        except BaseException:
            fault = [True]

        def wrapped(*args, **kwargs):
            # Foreign calls never inspect or mutate the shared sample.
            try:
                owned = bool(get_ident() == owner_thread)
            except BaseException:
                fault[0] = True
                return target(*args, **kwargs)
            if not owned:
                return target(*args, **kwargs)
            token = None
            depth = None
            try:
                depth = sample.depth
                token = sample.enter(category)
            except BaseException:
                fault[0] = True
            returned = False
            try:
                result = target(*args, **kwargs)
                returned = True
                return result
            finally:
                try:
                    try:
                        sample.leave(token, returned)
                    finally:
                        # Even a lost token drains without another clock.
                        if depth is not None:
                            sample.depth = depth
                    sample.observe_faults()
                except BaseException:
                    fault[0] = True
                    try:
                        sample.observe_faults()
                    except BaseException:
                        pass

        return wrapped


class _ElapsedLease:
    """Lease only the saved child call and the raw snapshot descriptor."""

    def __init__(self, module, fixture, sample, get_ident):
        self.module = module
        self.fixture = fixture
        self.sample = sample
        self.saved_output = module.check_output
        self.saved_cache = fixture.__dict__["cache_state"]
        owner = get_ident()
        if type(owner) is not int or not 0 <= owner <= sample.MAX:
            raise ValueError("invalid owner identity")
        self.output = sample.wrap(self.saved_output, 0, owner, get_ident)
        self.cache = sample.wrap(self.saved_cache, 5, owner, get_ident)

    def install(self):
        try:
            self.module.check_output = self.output
            self.fixture.cache_state = self.cache
        except BaseException:
            self.sample.valid = False
            self.restore()
        self.sample.start()

    def restore(self):
        # Reverse order; refusing foreign replacements is deliberate.
        released = True
        for obj, name, wrapper, saved in (
                (self.fixture, "cache_state", self.cache, self.saved_cache),
                (self.module, "check_output", self.output, self.saved_output)):
            try:
                current = obj.__dict__[name]
                if current is wrapper:
                    setattr(obj, name, saved)
                elif current is not saved:
                    self.sample.valid = False
            except BaseException:
                self.sample.valid = False
                released = False
        return released


def _elapsed_owner(test):
    # Read the four known default identities without adding test.id callbacks.
    if type(test) is CachedGitSafetyTests:
        return _CHECKPOINT_ORDINALS.get(
            f'__main__.CachedGitSafetyTests.{test._testMethodName}')
    return None


class _ElapsedDiagnostic:
    """First-sample elapsed diagnostics; missing coverage always stays unknown.

    G includes child launch, pipes, scheduling and waiting, not CPU or pure Git.
    S includes the entire snapshot, not just enumeration. Residual includes
    safety checks, fixtures, assertions, cleanup, scheduling and timer overhead.
    Top-level class fixtures are outside these selected lifecycles.
    """
    OWNERS = (11, 12, 20, 38)

    def __init__(self):
        self.seen = [False] * 4
        self.pending = [None] * 4
        self.lease = None
        self.owner = None

    def start(self, ordinal):
        if ordinal not in self.OWNERS:
            return
        index = self.OWNERS.index(ordinal)
        if self.seen[index]:
            # Repetition before emission invalidates the pending first sample.
            if self.owner == ordinal:
                self.abort()
            if self.pending[index] is not None:
                self.pending[index] = f'{ordinal}:first_only:unknown'
            return
        self.seen[index] = True
        if self.lease is not None:
            self.abort()
            return
        self.owner = ordinal
        self.lease = _ElapsedLease(subprocess, CachedGitSafetyTests,
                                   _ElapsedSample(time.monotonic_ns), threading.get_ident)
        self.lease.install()

    def stop(self, ordinal):
        if ordinal != self.owner or self.lease is None:
            return
        lease = self.lease
        released = False
        try:
            lease.sample.stop()
        finally:
            released = lease.restore()
        sample = lease.sample
        index = self.OWNERS.index(ordinal)
        # Completed owners retain only four <=756-byte strings, no samples.
        # Across all owners: 10 counters + 24 frames + 2 endpoints + last,
        # depth + validity + 2 latches + 4 seen flags + owner = 46 cells.
        # Four closure integer references and five constants make 55 cells.
        # Allow 64 transient integer locals: peak <=119, below 128;
        # there is never a stack array for each retained owner.
        try:
            sample.observe_faults()
            valid = (sample.valid and sample.depth == 0
                     and sample.counters[0] == sample.counters[1]
                     and sample.counters[5] == sample.counters[6])
            if valid:
                fields = []
                # Validate each of the ten <=19-byte fields before joining.
                for value in sample.counters:
                    if type(value) is not int or not 0 <= value <= sample.MAX:
                        raise ValueError('invalid elapsed counter')
                    field = str(value)
                    if type(field) is not type('') or field != f'{value:d}':
                        raise ValueError('invalid elapsed formatting')
                    fields.append(field)
                fields = ','.join(fields)
                residual = sample.life - sample.counters[4] - sample.counters[9]
                text = (f'{ordinal}:first_only:valid:life_ns={sample.life}'
                        f':G_S={fields}:residual_ns={residual}')
            else:
                text = f'{ordinal}:first_only:unknown'
            if len(text) > 756 or not text.isascii() or not text.isprintable():
                raise ValueError('elapsed summary exceeds fixed budget')
            self.pending[index] = text
        except BaseException:
            self.pending[index] = f'{ordinal}:first_only:unknown'
        finally:
            # Keep one failed lease available to the outer runner finally.
            if released:
                self.lease = None
                self.owner = None

    def suffix(self, ordinal, phase):
        if phase != 'stop' or ordinal not in (12, 20, 38):
            return ''
        indexes = (0, 1) if ordinal == 12 else (self.OWNERS.index(ordinal),)
        parts = [self.pending[index] for index in indexes if self.pending[index] is not None]
        if not parts:
            return ''
        text = ' elapsed_v1=' + ';'.join(parts)
        maximum = 1536 if ordinal == 12 else 768
        if len(text) > maximum:
            return ''
        for index in indexes:
            self.pending[index] = None
        return text

    def abort(self):
        if self.lease is not None:
            self.lease.sample.valid = False
            if self.lease.restore():
                self.lease = None
                self.owner = None


class ElapsedDiagnosticTests(unittest.TestCase):
    def test_exact_delegate_and_base_exceptions(self):
        for failure in (None, ValueError('target'), KeyboardInterrupt(), SystemExit(73)):
            args, value, calls = object(), object(), []

            def target(*positional, **keywords):
                calls.append((positional, keywords))
                if failure is not None:
                    raise failure
                return value

            clock = iter(range(100)).__next__
            sample = _ElapsedSample(clock)
            sample.start()
            wrapped = sample.wrap(target, 0, 1, lambda: 1)
            if failure is None:
                self.assertIs(wrapped(args, named=value), value)
            else:
                try:
                    wrapped(args, named=value)
                except BaseException as caught:
                    self.assertIs(caught, failure)
                else:
                    self.fail('target exception lost')
            sample.stop()
            self.assertEqual(len(calls), 1)
            self.assertIs(calls[0][0][0], args)
            self.assertIs(calls[0][1]['named'], value)
            self.assertEqual(sample.counters[:3], [1, 1, int(failure is not None)])
            self.assertTrue(sample.valid)

    def test_descriptor_actual_subclass_and_leaf_mock(self):
        calls = []

        class Fixture:
            def cache_state(self):
                calls.append(type(self))
                return self

        class Nested(Fixture):
            pass

        result = object()
        module = SimpleNamespace(check_output=lambda *a, **k: result)
        original = Fixture.cache_state
        sample = _ElapsedSample(iter(range(100)).__next__)
        lease = _ElapsedLease(module, Fixture, sample, lambda: 1)
        lease.install()
        obj = Nested()
        self.assertIs(obj.cache_state(), obj)
        leaf = module.check_output
        events = []

        def trace(*a, **k):
            events.append('prepare')
            self.assertEqual(sample.depth, 0)
            value = leaf(*a, **k)
            events.append('after')
            self.assertEqual(sample.depth, 0)
            return value

        module.check_output = trace
        self.assertIs(module.check_output('argv'), result)
        module.check_output = leaf
        lease.restore()
        sample.stop()
        self.assertEqual(calls, [Nested])
        self.assertEqual(events, ['prepare', 'after'])
        self.assertIs(Fixture.cache_state, original)
        self.assertEqual(sample.counters[:2], [1, 1])
        self.assertEqual(sample.counters[5:7], [1, 1])

    def test_nested_union_and_fault_drain(self):
        sample = _ElapsedSample(iter([0, 2, 3, 8, 10, 20]).__next__)
        sample.start()
        outer = sample.enter(5)
        inner = sample.enter(0)
        sample.leave(inner, True)
        sample.leave(outer, True)
        sample.stop()
        self.assertTrue(sample.valid)
        self.assertEqual((sample.counters[3], sample.counters[4]), (5, 5))
        self.assertEqual((sample.counters[8], sample.counters[9]), (8, 3))
        self.assertEqual(sample.life - sample.counters[4] - sample.counters[9], 12)
        sample = _ElapsedSample(iter(range(100)).__next__)
        sample.start()
        real_enter = sample.enter

        def faulty(category):
            real_enter(category)
            raise RuntimeError('lost enter token')

        sample.enter = faulty
        target = object()
        self.assertIs(sample.wrap(lambda: target, 0, 1, lambda: 1)(), target)
        self.assertEqual(sample.depth, 0)
        self.assertFalse(sample.valid)

    def test_foreign_call_has_no_sample_access(self):
        class Forbidden:
            def __getattribute__(self, name):
                raise AssertionError('foreign timer state access')

        value = object()
        wrapper = _ElapsedSample.wrap(Forbidden(), lambda: value, 0, 1, lambda: 2)
        self.assertIs(wrapper(), value)

    def test_clock_faults_and_counter_limits_keep_target(self):
        for clock in (lambda: -1, lambda: 1 << 63, lambda: True,
                      lambda: (_ for _ in ()).throw(KeyboardInterrupt()),
                      lambda: (_ for _ in ()).throw(SystemExit(73)),
                      lambda: (_ for _ in ()).throw(OSError('timer unavailable'))):
            sample = _ElapsedSample(clock)
            sample.start()
            value = object()
            self.assertIs(sample.wrap(lambda: value, 0, 1, lambda: 1)(), value)
            sample.stop()
            self.assertFalse(sample.valid)
        sample = _ElapsedSample(iter(range(9000)).__next__)
        sample.start()
        calls = []
        wrapper = sample.wrap(lambda: calls.append(1), 0, 1, lambda: 1)
        for _ in range(4097):
            wrapper()
        self.assertEqual(len(calls), 4097)
        self.assertEqual(sample.counters[0], 4096)
        self.assertFalse(sample.valid)
        self.assertEqual(sample.depth, 0)
        sample = _ElapsedSample(iter(range(100)).__next__)
        tokens = [sample.enter(0) for _ in range(9)]
        self.assertIsNone(tokens[-1])
        for token in reversed(tokens):
            sample.leave(token, True)
        self.assertEqual(sample.depth, 0)
        self.assertFalse(sample.valid)

    def test_partial_install_and_foreign_replacement(self):
        failure = RuntimeError('install fault')

        class Meta(type):
            def __setattr__(cls, name, value):
                if name == 'cache_state':
                    raise failure
                super().__setattr__(name, value)

        class Refusing(metaclass=Meta):
            def cache_state(self):
                return self

        saved = lambda: None
        module = SimpleNamespace(check_output=saved)
        sample = _ElapsedSample(iter(range(100)).__next__)
        lease = _ElapsedLease(module, Refusing, sample, lambda: 1)
        lease.install()
        self.assertIs(module.check_output, saved)
        self.assertIs(Refusing.__dict__['cache_state'], lease.saved_cache)
        self.assertFalse(sample.valid)

        class Fixture:
            def cache_state(self):
                return self

        lease = _ElapsedLease(module, Fixture, _ElapsedSample(iter(range(100)).__next__), lambda: 1)
        lease.install()
        foreign = lambda: None
        module.check_output = foreign
        lease.restore()
        lease.restore()
        self.assertIs(module.check_output, foreign)
        self.assertIs(Fixture.__dict__['cache_state'], lease.saved_cache)
        self.assertFalse(lease.sample.valid)

    def test_negative_overflow_missing_endpoint_and_faulted_exit(self):
        for stamps in ([10, 20, 19, 30], [0, 0, 10, 5]):
            sample = _ElapsedSample(iter(stamps).__next__)
            sample.start()
            token = sample.enter(0)
            sample.leave(token, True)
            sample.stop()
            self.assertFalse(sample.valid)
        sample = _ElapsedSample(iter(range(100)).__next__)
        sample.start()
        sample.counters[3] = sample.MAX
        token = sample.enter(0)
        sample.leave(token, True)
        self.assertFalse(sample.valid)
        self.assertEqual(sample.depth, 0)
        sample = _ElapsedSample(lambda: 100)
        sample.stop()
        self.assertFalse(sample.valid)
        sample = _ElapsedSample(iter(range(100)).__next__)
        sample.start()

        def broken(*args):
            raise RuntimeError('exit bookkeeping fault')

        sample.leave = broken
        target_error = KeyboardInterrupt()

        def target():
            raise target_error

        try:
            sample.wrap(target, 0, 1, lambda: 1)()
        except BaseException as caught:
            self.assertIs(caught, target_error)
        else:
            self.fail('target interrupt lost')
        self.assertEqual(sample.depth, 0)
        self.assertFalse(sample.valid)

    def test_pending_repetition_and_output_envelope(self):
        class Fixture:
            def cache_state(self):
                return self

        saved = lambda: b'value'
        module = SimpleNamespace(check_output=saved)
        clock_reads = []

        def clock():
            clock_reads.append(None)
            return len(clock_reads)

        with patch.dict(globals(), {'subprocess': module, 'CachedGitSafetyTests': Fixture,
                                   'time': SimpleNamespace(monotonic_ns=clock)}):
            diagnostic = _ElapsedDiagnostic()
            self.assertEqual(diagnostic.suffix(20, 'stop'), '')
            for owner in (11, 12, 20, 38):
                diagnostic.start(owner)
                for _ in range(4096):
                    module.check_output()
                diagnostic.stop(owner)
                self.assertIs(module.check_output, saved)
                if owner == 11:
                    self.assertEqual(diagnostic.suffix(11, 'stop'), '')
                else:
                    text = diagnostic.suffix(owner, 'stop')
                    self.assertIn('first_only:valid:', text)
                    self.assertTrue(text.isascii())
                    self.assertNotIn('\n', text)
                    self.assertLessEqual(len(text), 1536 if owner == 12 else 768)
                    self.assertEqual(diagnostic.suffix(owner, 'stop'), '')
            self.assertEqual(len(clock_reads), 32776)
            diagnostic.start(20)
            self.assertIsNone(diagnostic.lease)
            self.assertEqual(diagnostic.suffix(20, 'stop'), '')
            self.assertEqual(len(clock_reads), 32776)
            diagnostic.abort()
            missing = _ElapsedDiagnostic()
            missing.start(38)
            missing.abort()
            self.assertIs(module.check_output, saved)
            self.assertEqual(missing.suffix(38, 'stop'), '')

    def test_original_stream_error_and_callback_sequence(self):
        owner_name = 'test_fixture_commits_disable_maintenance_in_real_git'
        events = []

        class Fixture(unittest.TestCase):
            def cache_state(self):
                return self

            def id(self):
                events.append('id')
                return '__main__.CachedGitSafetyTests.' + owner_name

        saved = subprocess.check_output
        with patch.dict(globals(), {'CachedGitSafetyTests': Fixture}):
            case = Fixture()
            case._testMethodName = owner_name
            result = _LifecycleResult(io.StringIO(), True, 0)
            result.startTest(case)
            self.assertIsNot(subprocess.check_output, saved)
            result.stopTest(case)
            self.assertIs(subprocess.check_output, saved)
            self.assertEqual(events, ['id', 'id'])
            failure = OSError('original stream error')

            class Stream:
                def write(self, value):
                    raise failure

            result = _LifecycleResult(Stream(), True, 0)
            try:
                result.startTest(case)
            except OSError as caught:
                self.assertIs(caught, failure)
            else:
                self.fail('original stream failure lost')
            self.assertIs(subprocess.check_output, saved)

    def test_runner_finally_restores_after_target_interrupt(self):
        owner_name = 'test_fixture_commits_disable_maintenance_in_real_git'

        class Fixture(unittest.TestCase):
            def cache_state(self):
                return self

            def id(self):
                return '__main__.CachedGitSafetyTests.' + owner_name

        case = Fixture()
        case._testMethodName = owner_name
        failure = SystemExit(73)
        saved = subprocess.check_output

        def interrupted(runner, suite):
            result = runner._makeResult()
            result.startTest(case)
            raise failure

        with patch.dict(globals(), {'CachedGitSafetyTests': Fixture}), patch.object(
                unittest.TextTestRunner, 'run', interrupted):
            runner = _LifecycleRunner(stream=io.StringIO(), verbosity=0)
            try:
                runner.run(None)
            except SystemExit as caught:
                self.assertIs(caught, failure)
            else:
                self.fail('runner interrupt lost')
        self.assertIs(subprocess.check_output, saved)

    def test_callback_and_formatter_failures_restore_slots(self):
        owner_name = 'test_fixture_commits_disable_maintenance_in_real_git'

        class Fixture(unittest.TestCase):
            def cache_state(self):
                return self

            def id(self):
                return '__main__.CachedGitSafetyTests.' + owner_name

        case = Fixture()
        case._testMethodName = owner_name
        saved_output = subprocess.check_output
        saved_cache = Fixture.__dict__['cache_state']
        with patch.dict(globals(), {'CachedGitSafetyTests': Fixture}):
            for callback in ('startTest', 'stopTest'):
                failure = RuntimeError('original callback failure')
                result = _LifecycleResult(io.StringIO(), True, 0)
                if callback == 'stopTest':
                    result.startTest(case)
                with patch.object(unittest.TextTestResult, callback, side_effect=failure) as original:
                    try:
                        getattr(result, callback)(case)
                    except RuntimeError as caught:
                        self.assertIs(caught, failure)
                    else:
                        self.fail('callback error lost')
                    original.assert_called_once_with(case)
                self.assertIs(subprocess.check_output, saved_output)
                self.assertIs(Fixture.__dict__['cache_state'], saved_cache)
            diagnostic = _ElapsedDiagnostic()
            diagnostic.start(20)

            def formatter_fault(value):
                raise ValueError('formatter failure')

            with patch.dict(globals(), {'str': formatter_fault}):
                diagnostic.stop(20)
            self.assertEqual(diagnostic.suffix(20, 'stop'), ' elapsed_v1=20:first_only:unknown')
            self.assertIs(subprocess.check_output, saved_output)
            self.assertIs(Fixture.__dict__['cache_state'], saved_cache)

    def test_target_and_cleanup_errors_keep_original_lifecycle(self):
        owner_name = 'test_fixture_commits_disable_maintenance_in_real_git'
        sequences = []
        body_error = ValueError('body failure')
        cleanup_error = OSError('cleanup failure')

        class Fixture(unittest.TestCase):
            def cache_state(self):
                return self

            def id(self):
                return '__main__.CachedGitSafetyTests.' + owner_name

            def setUp(self):
                sequences.append('setup')
                self.addCleanup(self.clean)

            def clean(self):
                sequences.append('cleanup')
                raise cleanup_error

            def tearDown(self):
                sequences.append('teardown')

        def body(self):
            sequences.append('body')
            raise body_error

        setattr(Fixture, owner_name, body)
        saved_output = subprocess.check_output
        saved_cache = Fixture.__dict__['cache_state']
        with patch.dict(globals(), {'CachedGitSafetyTests': Fixture}):
            for result_type in (unittest.TextTestResult, _LifecycleResult):
                result = result_type(io.StringIO(), True, 0)
                # A decorator supplies unittest's standard writeln interface.
                result.stream = unittest.runner._WritelnDecorator(result.stream)
                Fixture(owner_name).run(result)
                self.assertEqual(len(result.errors), 2)
                self.assertIs(subprocess.check_output, saved_output)
                self.assertIs(Fixture.__dict__['cache_state'], saved_cache)
        self.assertEqual(sequences, ['setup', 'body', 'teardown', 'cleanup'] * 2)

    def test_flush_error_is_not_swallowed(self):
        owner_name = 'test_fixture_commits_disable_maintenance_in_real_git'
        failure = OSError('flush failure')
        writes = []

        class Stream:
            def write(self, text):
                writes.append(text)

            def flush(self):
                raise failure

        class Fixture(unittest.TestCase):
            def cache_state(self):
                return self

            def id(self):
                return '__main__.CachedGitSafetyTests.' + owner_name

        case = Fixture()
        case._testMethodName = owner_name
        saved = subprocess.check_output
        with patch.dict(globals(), {'CachedGitSafetyTests': Fixture}):
            result = _LifecycleResult(Stream(), True, 0)
            try:
                result.startTest(case)
            except OSError as caught:
                self.assertIs(caught, failure)
            else:
                self.fail('flush error lost')
        self.assertIs(subprocess.check_output, saved)
        self.assertEqual(len(writes), 1)

    def test_thread_probe_failure_makes_coverage_unknown(self):
        sample = _ElapsedSample(iter(range(100)).__next__)
        sample.start()
        value = object()
        calls = []

        def target():
            calls.append(None)
            return value

        def probe():
            raise RuntimeError('ownership bookkeeping unavailable')

        wrapped = sample.wrap(target, 0, 1, probe)
        self.assertIs(wrapped(), value)
        sample.stop()
        self.assertEqual(calls, [None])
        self.assertFalse(sample.valid, 'unobserved target call must not become valid zero')


    def test_probe_fault_latches_preserve_owner_and_foreign_outcomes(self):
        for category in (0, 5):
            for identity in (1, 2):
                for failure in (None, ValueError('target'), KeyboardInterrupt(), SystemExit(73)):
                    sample = _ElapsedSample(iter(range(100)).__next__)
                    sample.start()
                    value, argument, calls = object(), object(), []
                    broken = True

                    def probe():
                        if broken:
                            raise OSError('ownership unavailable')
                        return identity

                    def target(*args, **kwargs):
                        calls.append((args, kwargs))
                        if failure is not None:
                            raise failure
                        return value

                    wrapped = sample.wrap(target, category, 1, probe)
                    if failure is None:
                        self.assertIs(wrapped(argument, named=value), value)
                    else:
                        try:
                            wrapped(argument, named=value)
                        except BaseException as caught:
                            self.assertIs(caught, failure)
                        else:
                            self.fail('target exception lost')
                    self.assertEqual(len(calls), 1)
                    self.assertIs(calls[0][0][0], argument)
                    self.assertIs(calls[0][1]['named'], value)
                    self.assertEqual(sample.counters, [0] * 10)
                    self.assertEqual(sample.depth, 0)
                    self.assertTrue(sample.fault_latches[category // 5][0])
                    broken = False
                    if failure is None:
                        self.assertIs(wrapped(argument, named=value), value)
                    else:
                        with self.assertRaises(type(failure)) as caught:
                            wrapped(argument, named=value)
                        self.assertIs(caught.exception, failure)
                    sample.stop()
                    self.assertFalse(sample.valid)
                    self.assertTrue(sample.fault_latches[category // 5][0])
                    self.assertEqual(len(calls), 2)

    def test_unknown_probe_only_touches_its_prebound_latch(self):
        class Sample(_ElapsedSample):
            blocked = False

            def __getattribute__(self, name):
                if name != 'blocked' and object.__getattribute__(self, 'blocked'):
                    raise AssertionError('unknown ownership accessed sample')
                return super().__getattribute__(name)

        sample = Sample(iter(range(100)).__next__)
        sample.start()
        value = object()
        calls = []

        def probe():
            raise RuntimeError('unknown ownership')

        def target():
            calls.append(None)
            return value

        wrapped = sample.wrap(target, 0, 1, probe)
        latch = sample.fault_latches[0]
        sample.blocked = True
        self.assertIs(wrapped(), value)
        sample.blocked = False
        self.assertEqual(calls, [None])
        self.assertTrue(latch[0])
        sample.stop()
        self.assertFalse(sample.valid)

    def test_latches_registered_before_install_and_consumed_after_restore(self):
        class Fixture:
            def cache_state(self):
                return self

        saved = lambda: object()
        module = SimpleNamespace(check_output=saved)
        sample = _ElapsedSample(iter(range(100)).__next__)
        lease = _ElapsedLease(module, Fixture, sample, lambda: 1)
        for wrapper, latch in zip((lease.output, lease.cache), sample.fault_latches):
            captured = dict(zip(wrapper.__code__.co_freevars,
                                (cell.cell_contents for cell in wrapper.__closure__)))
            self.assertIs(captured['fault'], latch)
            self.assertFalse(latch[0])
        lease.install()
        sample.fault_latches[1][0] = True
        lease.restore()
        lease.restore()
        sample.stop()
        self.assertIs(module.check_output, saved)
        self.assertIs(Fixture.__dict__['cache_state'], lease.saved_cache)
        self.assertFalse(sample.valid)
        self.assertTrue(sample.fault_latches[1][0])

    def test_bookkeeping_read_fault_preserves_target_exception(self):
        class Sample(_ElapsedSample):
            blocked = False

            def __getattribute__(self, name):
                if name == 'depth' and self.blocked:
                    raise OSError('depth unavailable')
                return super().__getattribute__(name)

        for failure in (None, KeyboardInterrupt(), SystemExit(73)):
            sample = Sample(iter(range(100)).__next__)
            sample.start()
            value, calls = object(), []

            def target():
                calls.append(None)
                if failure is not None:
                    raise failure
                return value

            wrapped = sample.wrap(target, 0, 1, lambda: 1)
            sample.blocked = True
            if failure is None:
                self.assertIs(wrapped(), value)
            else:
                with self.assertRaises(type(failure)) as caught:
                    wrapped()
                self.assertIs(caught.exception, failure)
            sample.blocked = False
            sample.stop()
            self.assertEqual(calls, [None])
            self.assertFalse(sample.valid)

    def test_repetition_before_emission_and_missing_lifecycle_stay_unknown(self):
        class Fixture:
            def cache_state(self):
                return self

        saved = lambda: b'value'
        module = SimpleNamespace(check_output=saved)
        with patch.dict(globals(), {'subprocess': module, 'CachedGitSafetyTests': Fixture}):
            diagnostic = _ElapsedDiagnostic()
            diagnostic.start(11)
            diagnostic.stop(11)
            diagnostic.start(11)
            self.assertEqual(diagnostic.suffix(12, 'stop'), ' elapsed_v1=11:first_only:unknown')
            diagnostic.start(20)
            sample = diagnostic.lease.sample
            diagnostic.start(20)
            self.assertIsNone(diagnostic.lease)
            self.assertFalse(sample.valid)
            self.assertIs(module.check_output, saved)
            self.assertEqual(diagnostic.suffix(20, 'stop'), '')
            diagnostic.start(38)
            diagnostic.abort()
            diagnostic.stop(38)
            self.assertEqual(diagnostic.suffix(38, 'stop'), '')
            self.assertIs(module.check_output, saved)

    def test_four_retained_owners_share_one_bounded_stack(self):
        class Fixture:
            def cache_state(self):
                return self

        module = SimpleNamespace(check_output=lambda: None)
        visited = set()

        def cells(value):
            if type(value) in (int, bool):
                return 1
            if isinstance(value, (_ElapsedDiagnostic, _ElapsedLease, _ElapsedSample, list, tuple)):
                if id(value) in visited:
                    return 0
                visited.add(id(value))
                values = value if isinstance(value, (list, tuple)) else value.__dict__.values()
                return sum(cells(item) for item in values)
            if getattr(value, '__name__', None) == 'wrapped':
                if id(value) in visited:
                    return 0
                visited.add(id(value))
                return sum(cells(cell.cell_contents) for name, cell in
                           zip(value.__code__.co_freevars, value.__closure__)
                           if name not in ('get_ident', 'target'))
            return 0

        with patch.dict(globals(), {'subprocess': module, 'CachedGitSafetyTests': Fixture,
                                   'time': SimpleNamespace(monotonic_ns=iter(range(1000)).__next__)}):
            diagnostic = _ElapsedDiagnostic()
            for owner in (11, 12, 20):
                diagnostic.start(owner)
                diagnostic.stop(owner)
            self.assertTrue(all(isinstance(item, str) for item in diagnostic.pending[:3]))
            diagnostic.start(38)
            sample = diagnostic.lease.sample
            tokens = [sample.enter(0 if index % 2 else 5) for index in range(8)]
            self.assertEqual(sample.depth, 8)
            # Reserve one cell for the not-yet-written lifecycle endpoint,
            # five fixed owner/MAX constants, and 64 transient local cells.
            self.assertLessEqual(cells(diagnostic) + 1 + 5 + 64, 128)
            self.assertEqual(len(sample.frames), 8)
            for token in reversed(tokens):
                sample.leave(token, True)
            diagnostic.stop(38)
            self.assertIsNone(diagnostic.lease)
            self.assertTrue(all(isinstance(item, str) for item in diagnostic.pending))
            self.assertLessEqual(sum(len(item) for item in diagnostic.pending), 4 * 756)

    def test_fault_summary_has_no_zero_measurements(self):
        class Fixture:
            def cache_state(self):
                return self

        module = SimpleNamespace(check_output=lambda: object())
        with patch.dict(globals(), {'subprocess': module, 'CachedGitSafetyTests': Fixture}):
            diagnostic = _ElapsedDiagnostic()
            diagnostic.start(20)
            lease = diagnostic.lease
            with patch.object(lease, 'restore', wraps=lease.restore) as restore:
                lease.sample.fault_latches[0][0] = True
                diagnostic.stop(20)
                restore.assert_called_once_with()
            self.assertEqual(diagnostic.suffix(20, 'stop'), ' elapsed_v1=20:first_only:unknown')
            self.assertEqual(diagnostic.suffix(20, 'stop'), '')


    def test_failed_fault_observation_never_masks_target(self):
        for failure in (None, KeyboardInterrupt(), SystemExit(73)):
            sample = _ElapsedSample(iter(range(100)).__next__)
            sample.start()
            value, calls = object(), []

            def target():
                calls.append(None)
                if failure is not None:
                    raise failure
                return value

            with patch.object(sample, 'leave', side_effect=OSError('bookkeeping')), patch.object(
                    sample, 'observe_faults', side_effect=OSError('observation')):
                wrapped = sample.wrap(target, 0, 1, lambda: 1)
                if failure is None:
                    self.assertIs(wrapped(), value)
                else:
                    with self.assertRaises(type(failure)) as caught:
                        wrapped()
                    self.assertIs(caught.exception, failure)
            sample.stop()
            self.assertEqual(calls, [None])
            self.assertEqual(sample.depth, 0)
            self.assertFalse(sample.valid)


    def test_transient_restore_failure_retains_lease_for_outer_finally(self):
        deny_restore = [True]

        class Meta(type):
            def __setattr__(cls, name, value):
                if name == 'cache_state' and value is saved_cache and deny_restore[0]:
                    deny_restore[0] = False
                    raise OSError('transient restoration failure')
                super().__setattr__(name, value)

        class Fixture(metaclass=Meta):
            def cache_state(self):
                return self

        saved_cache = Fixture.__dict__['cache_state']
        saved_output = lambda: b'value'
        module = SimpleNamespace(check_output=saved_output)
        with patch.dict(globals(), {'subprocess': module, 'CachedGitSafetyTests': Fixture}):
            diagnostic = _ElapsedDiagnostic()
            try:
                diagnostic.start(20)
                sample = diagnostic.lease.sample
                diagnostic.stop(20)
                self.assertIs(module.check_output, saved_output)
                self.assertIsNot(Fixture.__dict__['cache_state'], saved_cache)
                self.assertIsNotNone(diagnostic.lease)
                self.assertFalse(sample.valid)
                self.assertEqual(diagnostic.suffix(20, 'stop'), ' elapsed_v1=20:first_only:unknown')
                diagnostic.abort()
                self.assertIsNone(diagnostic.lease)
                self.assertIs(Fixture.__dict__['cache_state'], saved_cache)
            finally:
                # This control owns its synthetic class even on rejection.
                Fixture.cache_state = saved_cache
                diagnostic.abort()


    def test_clock_reversal_between_intervals_is_unknown(self):
        for stamps in ([100, 90, 95, 200], [0, 2, 4, 3, 5, 10]):
            sample = _ElapsedSample(iter(stamps).__next__)
            sample.start()
            value, calls = object(), []

            def target():
                calls.append(None)
                return value

            wrapped = sample.wrap(target, 0, 1, lambda: 1)
            self.assertIs(wrapped(), value)
            if len(stamps) == 6:
                self.assertIs(wrapped(), value)
            sample.stop()
            self.assertFalse(sample.valid)
            self.assertEqual(len(calls), 2 if len(stamps) == 6 else 1)

    def test_malformed_formatter_never_emits_valid_or_extra_lines(self):
        class Fixture:
            def cache_state(self):
                return self

        module = SimpleNamespace(check_output=lambda: None)
        with patch.dict(globals(), {'subprocess': module, 'CachedGitSafetyTests': Fixture}):
            for bad in ('\n', '\r', '\x00', 'x', '', '1', '0' * 769):
                diagnostic = _ElapsedDiagnostic()
                diagnostic.start(20)
                with patch.dict(globals(), {'str': lambda value: bad}):
                    diagnostic.stop(20)
                self.assertEqual(diagnostic.suffix(20, 'stop'), ' elapsed_v1=20:first_only:unknown')
                self.assertIsNone(diagnostic.lease)


    def test_ownership_truth_conversion_fault_latches_before_delegate(self):
        class Truth:
            def __bool__(self):
                raise OSError('ownership comparison unavailable')

        class Identity:
            def __eq__(self, other):
                return Truth()

        sample = _ElapsedSample(iter(range(100)).__next__)
        sample.start()
        calls, value = [], object()

        def target():
            calls.append(None)
            return value

        self.assertIs(sample.wrap(target, 0, 1, Identity)(), value)
        self.assertEqual(calls, [None])
        sample.stop()
        self.assertFalse(sample.valid)


    def test_owner_lookup_fault_preserves_original_callbacks(self):
        owner_name = 'test_fixture_commits_disable_maintenance_in_real_git'

        class Fixture(unittest.TestCase):
            def cache_state(self):
                return self

            def id(self):
                return '__main__.CachedGitSafetyTests.' + owner_name

        case = Fixture()
        case._testMethodName = owner_name
        saved = subprocess.check_output

        def lookup_fault(test):
            raise OSError('owner lookup unavailable')

        with patch.dict(globals(), {'CachedGitSafetyTests': Fixture}):
            for callback in ('startTest', 'stopTest'):
                result = _LifecycleResult(io.StringIO(), True, 0)
                original = getattr(unittest.TextTestResult, callback)
                try:
                    if callback == 'stopTest':
                        result.startTest(case)
                    with patch.dict(globals(), {'_elapsed_owner': lookup_fault}), patch.object(
                            unittest.TextTestResult, callback, autospec=True, side_effect=original) as observed:
                        getattr(result, callback)(case)
                        observed.assert_called_once_with(result, case)
                    self.assertIs(subprocess.check_output, saved)
                    self.assertIsNone(result._elapsed.lease)
                finally:
                    result._diagnostic('abort')


    def test_owner_identity_is_bounded_before_installation(self):
        owner_name = 'test_fixture_commits_disable_maintenance_in_real_git'

        class Fixture(unittest.TestCase):
            def cache_state(self):
                return self

            def id(self):
                return '__main__.CachedGitSafetyTests.' + owner_name

        case = Fixture()
        case._testMethodName = owner_name
        saved_output = subprocess.check_output
        saved_cache = Fixture.__dict__['cache_state']
        with patch.dict(globals(), {'CachedGitSafetyTests': Fixture}):
            for owner in (-1, True, 1 << 63, None, 0, (1 << 63) - 1):
                result = _LifecycleResult(io.StringIO(), True, 0)
                with patch.dict(globals(), {'threading': SimpleNamespace(get_ident=lambda: owner)}):
                    try:
                        result.startTest(case)
                        if type(owner) is int and 0 <= owner <= (1 << 63) - 1:
                            self.assertIsNotNone(result._elapsed.lease)
                        else:
                            self.assertIsNone(result._elapsed.lease)
                            self.assertIs(subprocess.check_output, saved_output)
                            self.assertIs(Fixture.__dict__['cache_state'], saved_cache)
                        result.stopTest(case)
                    finally:
                        result._diagnostic('abort')
                self.assertIs(subprocess.check_output, saved_output)
                self.assertIs(Fixture.__dict__['cache_state'], saved_cache)


class ImportModeTests(unittest.TestCase):
    def test_isolated_import_has_no_startup_output_and_runs_diagnostic_control(self):
        spec = importlib.util.spec_from_file_location("offline_deps_import_test", __file__)
        module = importlib.util.module_from_spec(spec)
        output, diagnostic = io.StringIO(), io.StringIO()
        with patch.object(sys, "stdout", output), redirect_stderr(diagnostic):
            spec.loader.exec_module(module)
        self.assertEqual(output.getvalue(), "")
        self.assertEqual(diagnostic.getvalue(), "")
        # Select one fixture-free control; never collect the imported suite.
        result = unittest.TestResult()
        module.ElapsedDiagnosticTests("test_original_stream_error_and_callback_sequence").run(result)
        self.assertEqual(result.testsRun, 1)
        self.assertEqual(result.errors, [])
        self.assertEqual(result.failures, [])
        self.assertEqual(result.skipped, [])
        self.assertIs(module.time, time)
        self.assertIs(module._LifecycleRunner.resultclass, module._LifecycleResult)
        self.assertEqual(module._CHECKPOINT_ORDINALS, _CHECKPOINT_ORDINALS)
        self.assertEqual(module._START_ORDINALS, _START_ORDINALS)
        self.assertEqual(module._STOP_ORDINALS, _STOP_ORDINALS)


# Fixed source identities keep checkpoints stable under unittest selection.
_CHECKPOINT_ORDINALS = {
    "__main__.CachedGitSafetyTests.test_alternate_and_common_directory_cycles_are_refused_before_git": 1,
    "__main__.CachedGitSafetyTests.test_config_includes_are_refused_in_all_metadata_configs_before_git_runs": 11,
    "__main__.CachedGitSafetyTests.test_control_whitespace_includes_refuse_every_cached_git_entry_point": 12,
    "__main__.CachedGitSafetyTests.test_fixture_commits_disable_maintenance_in_real_git": 20,
    "__main__.CachedGitSafetyTests.test_real_recursive_gitlinks_check_initialized_heads": 38,
    "__main__.CachedGitSafetyTests.test_windows_crlf_alternate_metadata_is_contained_on_every_host": 45,
    "__main__.OfflineDependenciesTests.test_candidate_cleanup_errors_after_publication_keep_stage_and_original_error": 49,
    "__main__.OfflineDependenciesTests.test_glad_requires_bundled_generator_and_specification_inputs": 65,
    "__main__.OfflineDependenciesTests.test_initial_cache_replace_failure_cleans_up_and_retry_succeeds": 66,
    "__main__.OfflineDependenciesTests.test_symlinked_scratch_root_refused_without_writes": 97,
    "__main__.OfflineDependenciesTests.test_uncertain_publication_retains_stage_without_claiming_an_occupant": 98,
    "__main__.WindowsReparseMetadataTests.test_original_components_are_checked_before_dotdot_resolution": 120,
}
_START_ORDINALS = frozenset((1, 11, 20, 38, 49, 66, 98))
_STOP_ORDINALS = frozenset((12, 20, 38, 45, 65, 66, 97, 98, 120))

class _LifecycleResult(unittest.TextTestResult):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        # At most 16 phase/identity pairs, including repeated selections.
        self._emitted_checkpoints = set()
        self._elapsed = None
        try:
            self._elapsed = _ElapsedDiagnostic()
        except BaseException:
            pass

    def _diagnostic(self, method, *args):
        try:
            if self._elapsed is not None:
                if method in ('start', 'stop'):
                    args = (_elapsed_owner(args[0]),)
                return getattr(self._elapsed, method)(*args)
        except BaseException:
            try:
                self._elapsed.abort()
            except BaseException:
                pass
        return ''

    def _checkpoint(self, test, phase, selected):
        identity = test.id()
        ordinal = _CHECKPOINT_ORDINALS.get(identity)
        checkpoint = (phase, ordinal)
        if ordinal in selected and checkpoint not in self._emitted_checkpoints:
            if phase == "start" and self.showAll:
                self.stream.write("\n")
            self.stream.write(
                f"Offline dependencies: monotonic_ns={time.monotonic_ns()} "
                f"phase={phase} ordinal={ordinal} identity={identity}"
                f"{self._diagnostic('suffix', ordinal, phase)}\n"
            )
            self.stream.flush()
            self._emitted_checkpoints.add(checkpoint)

    def startTest(self, test):
        self._diagnostic('start', test)
        try:
            super().startTest(test)
            self._checkpoint(test, "start", _START_ORDINALS)
        except BaseException:
            self._diagnostic('abort')
            raise

    def stopTest(self, test):
        try:
            super().stopTest(test)
        finally:
            self._diagnostic('stop', test)
        self._checkpoint(test, "stop", _STOP_ORDINALS)

class _LifecycleRunner(unittest.TextTestRunner):
    resultclass = _LifecycleResult

    def _makeResult(self):
        result = super()._makeResult()
        self._elapsed_result = result
        return result

    def run(self, test):
        try:
            return super().run(test)
        finally:
            result = getattr(self, '_elapsed_result', None)
            if result is not None:
                result._diagnostic('abort')


if __name__ == "__main__":
    unittest.main(verbosity=2, testRunner=_LifecycleRunner)
