#!/usr/bin/env python3
"""Isolated stdlib contracts for offline dependency provisioning."""

import errno
import importlib.util
import json
import os
from pathlib import Path
import shlex
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("offline_deps", ROOT / "tools/ci/offline_deps.py")
deps = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(deps)


class OfflineDependenciesTests(unittest.TestCase):
    def setUp(self):
        (ROOT / "scratch").mkdir(exist_ok=True)
        temporary = tempfile.TemporaryDirectory(prefix="offline deps ", dir=ROOT / "scratch")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
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
        real_replace = deps.os.replace
        def fail(source, target):
            self.assertEqual(Path(source).parent, Path(target).parent)
            self.assertFalse(Path(target).exists())
            self.assertIn("FETCHCONTENT_SOURCE_DIR_SAMPLE", Path(source).read_text())
            raise OSError("injected cache publication failure")
        with patch.object(deps.os, "replace", side_effect=fail):
            with self.assertRaisesRegex(OSError, "publication failure"):
                self.stage()
        self.assertFalse(init.exists())
        self.assertFalse((self.root / "scratch/copied sources").exists())
        self.assertEqual(list(init.parent.glob(init.name + ".*.tmp")), [])
        with patch.object(deps.os, "replace", wraps=real_replace) as replace:
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

        with patch.object(deps.os, "replace", side_effect=fail):
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
        directory.chmod(mode | stat.S_IWUSR)
        try:
            for name, target in (("file link", content), ("directory link", directory)):
                try:
                    (directory / name).symlink_to(target.name if target == content else ".",
                                                  target_is_directory=target == directory)
                except OSError as error:
                    if (error.errno not in (errno.EPERM, errno.EACCES, errno.ENOSYS,
                                           errno.EOPNOTSUPP)
                            and getattr(error, "winerror", None) not in (1, 50, 1314)):
                        raise
                    self.skipTest(f"Symlink creation is unavailable: {error}")
        finally:
            directory.chmod(mode)
        self.write_lock()
        modes = [path.stat().st_mode for path in (directory, content)]
        real_copy = deps.shutil.copytree
        real_chmod, real_unlink, real_rmdir = os.chmod, os.unlink, os.rmdir

        for phase in ("publication", "copy"):
            with self.subTest(phase=phase):
                dest = self.root / "scratch" / phase
                init = self.root / "scratch" / (phase + ".cmake")
                original = OSError("injected " + phase + " failure")
                cleaning = False
                retried_links = set()

                def fail():
                    nonlocal cleaning
                    copied = dest / "sample/readonly directory"
                    self.assertEqual((copied / "file link").read_text(), "readonly content\n")
                    # Model links pointing outside the partial stage. Cleanup
                    # must neither traverse them nor chmod their cache targets.
                    real_chmod(copied, mode | stat.S_IWUSR)
                    for name, target in (("file link", content), ("directory link", directory)):
                        link = copied / name
                        link.unlink()
                        link.symlink_to(target, target_is_directory=target == directory)
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
                        patch.object(deps.os, "replace", side_effect=publish), \
                        patch.object(deps.os, "chmod", side_effect=chmod), \
                        patch.object(deps.os, "unlink", side_effect=lambda *a, **kw: remove(real_unlink, *a, **kw)), \
                        patch.object(deps.os, "rmdir", side_effect=lambda *a, **kw: remove(real_rmdir, *a, **kw)):
                    with self.assertRaises(OSError) as caught:
                        self.stage(dest, init)
                self.assertIs(caught.exception, original)
                self.assertFalse(dest.exists())
                self.assertFalse(init.exists())
                self.assertEqual(list(init.parent.glob(init.name + ".*.tmp")), [])
                self.assertEqual(retried_links, {"file link", "directory link"})
                self.assertEqual([path.stat().st_mode for path in (directory, content)], modes)
                self.stage(dest, init)
                copied = dest / "sample/readonly directory"
                self.assertTrue((copied / "file link").is_symlink())
                self.assertTrue((copied / "directory link").is_symlink())
                self.assertEqual((copied / "file link").read_text(), "readonly content\n")
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

                with patch.object(deps.os, "replace", side_effect=original), \
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


class CachedGitSafetyTests(unittest.TestCase):
    """Use real local Git repositories; no transports or native tools run."""

    def setUp(self):
        (ROOT / "scratch").mkdir(exist_ok=True)
        temporary = tempfile.TemporaryDirectory(prefix="cached git ", dir=ROOT / "scratch")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.root = self.base / "clone"
        (self.root / "vendor").mkdir(parents=True)
        (self.root / "vendor/CMakeLists.txt").write_text(
            "FetchContent_Declare(sample GIT_REPOSITORY https://example.org/sample.git GIT_TAG v1)\n")
        self.cache = self.base / "cache with spaces"
        self.source = self.cache / "sample"
        self.source.mkdir(parents=True)
        self.initialize(self.source)
        self.lock = self.root / "lock.json"
        self.write_lock()
        self.marker = self.base / "executed marker"
        script = self.base / "monitor script.py"
        script.write_text("import pathlib, sys\npathlib.Path(sys.argv[1]).write_text('executed')\n")
        # Git's configured commands use its shell on Windows too.
        self.command = shlex.join([Path(sys.executable).as_posix(), "-I", "-B",
                                   script.as_posix(), self.marker.as_posix()])

    def git(self, source, *args):
        return deps.git(source, *args, cache=self.cache)

    def initialize(self, source):
        self.git(source, "init", "--quiet")
        (source / "input.h").write_text("content\n")
        self.git(source, "add", "input.h")
        self.commit(source)

    def commit(self, source):
        self.git(source, "-c", "user.name=Fixture", "-c", "user.email=fixture@example.org",
                 "-c", "commit.gpgSign=false", "commit", "--quiet", "-m", "fixture")

    def write_lock(self):
        lock = deps.unresolved_lock(self.root)
        lock["status"] = "resolved"
        lock["dependencies"]["sample"].update(
            commit=deps.head(self.source, self.cache), sha256=deps.source_digest(self.source),
            submodules=deps.submodules(self.source, self.cache))
        self.lock.write_text(json.dumps(lock))

    def actions(self):
        return (lambda: deps.verify(self.root, self.cache, self.lock),
                lambda: deps.stage(self.root, self.cache, self.lock,
                                   self.root / "scratch/staged", self.root / "scratch/init.cmake"))

    def cache_state(self):
        return {p.relative_to(self.cache).as_posix():
                os.readlink(p) if p.is_symlink() else p.read_bytes()
                for p in self.cache.rglob("*") if p.is_symlink() or p.is_file()}

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
        self.write_alternates(common / "objects", str(outside))
        (metadata / "commondir").write_text(os.path.relpath(common, metadata) + "\n")
        with patch.object(deps.subprocess, "check_output", side_effect=AssertionError("Git ran")):
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
                self.git(self.source, "config", f"protocol.{protocol}.allow", "always")
                with self.assertRaises(subprocess.CalledProcessError) as refused:
                    self.git(self.source, "ls-remote", url)
                self.assertIn(f"transport '{protocol}' not allowed".encode(), refused.exception.stderr)
                self.assertFalse(self.marker.exists())

    def test_real_recursive_gitlinks_check_initialized_heads(self):
        nested = self.source / "nested path (v1)"
        nested.mkdir()
        self.initialize(nested)
        child = nested / "child"
        child.mkdir()
        self.initialize(child)
        child_head = deps.head(child, self.cache)
        self.git(nested, "update-index", "--add", "--cacheinfo", "160000", child_head, "child")
        self.commit(nested)
        nested_head = deps.head(nested, self.cache)
        self.git(self.source, "update-index", "--add", "--cacheinfo", "160000", nested_head, nested.name)
        self.commit(self.source)
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
                 "-c", "commit.gpgSign=false", "commit", "--quiet", "--allow-empty", "-m", "changed")
        # Keep the content digest correct while changing only submodule HEAD.
        self.assert_refused_without_writes()
        (child / ".git/HEAD").unlink()
        self.assert_refused_without_writes()

    def test_submodule_gitdir_outside_cache_is_refused(self):
        nested = self.source / "nested"
        nested.mkdir()
        self.initialize(nested)
        nested_head = deps.head(nested, self.cache)
        self.git(self.source, "update-index", "--add", "--cacheinfo", "160000", nested_head, "nested")
        self.commit(self.source)
        self.write_lock()
        metadata = self.base / "outside metadata"
        (nested / ".git").rename(metadata)
        (nested / ".git").write_text("gitdir: " + str(metadata) + "\n")
        self.assert_refused_without_writes()


if __name__ == "__main__":
    unittest.main()
