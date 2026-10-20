#!/usr/bin/env python3
"""Self-test for the canonical fixture hash."""

from __future__ import annotations

import contextlib
import errno
import hashlib
import io
import json
import ntpath
import os
import posixpath
import stat
import subprocess
import sys
import tempfile
import types
import unittest
from pathlib import Path, PurePosixPath, PureWindowsPath
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

import fixture_hash as module  # noqa: E402
import fixture_paths
from fixture_paths import reference_parts, resolve_reference


ROOTED_REFERENCES = (
    "/outside.json", "//server/share/outside.json",
    "C:/outside.json", "c:\\outside.json", "C:outside.json",
    "\\\\server\\share\\outside.json", "\\outside.json",
    "\\\\?\\C:\\outside.json", "\\\\.\\C:\\outside.json",
    "./C:/outside.json", "sub/../C:/outside.json", ".\\C:outside.json",
    "sub\\..\\C:\\outside.json",
)
ESCAPING_REFERENCES = (
    "../outside.json", "../../outside.json", "sub/../../outside.json",
    "..\\outside.json", "..\\..\\outside.json", "sub\\..\\..\\outside.json",
    "sub/..\\../outside.json",
)


def symlink_or_skip(test, link, target, *, directory=False):
    try:
        link.symlink_to(target, target_is_directory=directory)
    except NotImplementedError as exc:
        test.skipTest(f"OS cannot create symlinks: {exc}")
    except OSError as exc:
        if exc.errno not in (errno.EPERM, errno.EACCES, errno.ENOSYS, errno.ENOTSUP):
            raise
        test.skipTest(f"OS cannot create symlinks: {exc}")


def directory_link(link, target):
    """Junctions need no symlink privilege on Windows."""
    if os.name == "nt":
        subprocess.run(["cmd.exe", "/d", "/c", "mklink", "/J", str(link), str(target)],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    else:
        link.symlink_to(target, target_is_directory=True)


def remove_directory_link(link):
    if os.name == "nt":
        link.rmdir()
    else:
        link.unlink()


class FixtureReferenceTests(unittest.TestCase):
    def test_both_path_grammars_reject_rooted_references_without_io(self):
        with mock.patch.object(Path, "resolve", side_effect=AssertionError("unexpected I/O")):
            for reference in ROOTED_REFERENCES:
                with self.subTest(reference=reference):
                    with self.assertRaisesRegex(ValueError, "must be root-relative"):
                        resolve_reference(Path("unused"), reference)

    def test_both_path_grammars_reject_escapes_without_io(self):
        with mock.patch.object(Path, "resolve", side_effect=AssertionError("unexpected I/O")):
            for reference in ESCAPING_REFERENCES:
                with self.subTest(reference=reference):
                    with self.assertRaisesRegex(ValueError, "escapes root"):
                        resolve_reference(Path("unused"), reference)

    def test_normalized_in_root_references_and_empty_paths(self):
        self.assertEqual(reference_parts("common/./sub/../biomes.json"), ("common", "biomes.json"))
        for reference in ("", ".", "./", "sub/..", "sub\\..", "bad\0name"):
            with self.subTest(reference=reference), self.assertRaisesRegex(ValueError, "root-relative"):
                reference_parts(reference)

    def test_accepted_references_follow_each_native_path_grammar(self):
        references = (
            "extra\\biomes.json", "extra\\sub\\..\\biomes.json",
            "extra/./sub/../biomes.json", "extra\\sub/../biomes.json",
            "extra/sub/..\\biomes.json", "extra//biomes.json",
            r"a\b/../..\biomes.json",
        )
        for grammar, normalize in ((PurePosixPath, posixpath.normpath),
                                   (PureWindowsPath, ntpath.normpath)):
            root = grammar("fixtures")
            with mock.patch.object(fixture_paths, "Path", grammar), \
                    mock.patch.object(fixture_paths, "contain_path", side_effect=lambda r, p: p):
                for reference in references:
                    with self.subTest(grammar=grammar.__name__, reference=reference):
                        self.assertEqual(reference_parts(reference), grammar(normalize(reference)).parts)
                        self.assertEqual(resolve_reference(root, reference), root / normalize(reference))
                for reference in ROOTED_REFERENCES + ESCAPING_REFERENCES:
                    with self.subTest(grammar=grammar.__name__, refused=reference), \
                            self.assertRaisesRegex(ValueError, "fixture reference"):
                        resolve_reference(root, reference)

    def test_native_normalization_also_refuses_empty_and_escaping_paths(self):
        # The portable Windows interpretation counts two directory components;
        # POSIX sees one literal name containing a backslash.
        for reference, message in (("extra\\sub/..", "root-relative"),
                                   ("extra\\sub/../../biomes.json", "escapes root")):
            with self.subTest(reference=reference):
                with mock.patch.object(fixture_paths, "Path", PurePosixPath), \
                        self.assertRaisesRegex(ValueError, message):
                    reference_parts(reference)
                with mock.patch.object(fixture_paths, "Path", PureWindowsPath):
                    self.assertEqual(reference_parts(reference),
                                     PureWindowsPath(ntpath.normpath(reference)).parts)

    def test_native_prefixes_stream_in_order_without_collecting_ancestors(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            # No matching directories: exercise more than the hash-only budget
            # without introducing a new limit for the generic containment API.
            parts = ("m",) * 65
            original = Path.resolve
            depths = []

            def resolve(path, *args, **kwargs):
                depth = len(depths)
                self.assertEqual(path, root.joinpath(*parts[:depth]))
                depths.append(depth)
                return original(path, *args, **kwargs)

            with mock.patch.object(Path, "parents", new_callable=mock.PropertyMock,
                                   side_effect=AssertionError("collected ancestors")), \
                    mock.patch.object(Path, "resolve", resolve):
                self.assertEqual(fixture_paths.contain_path(root, root.joinpath(*parts)),
                                 root.joinpath(*parts))
            self.assertEqual(depths, list(range(len(parts) + 1)))

    def test_reference_depth_budget_uses_normalized_native_parts(self):
        for grammar in (PurePosixPath, PureWindowsPath):
            root = grammar("fixtures")
            with self.subTest(grammar=grammar.__name__), \
                    mock.patch.object(fixture_paths, "Path", grammar), \
                    mock.patch.object(fixture_paths, "contain_path", side_effect=lambda r, p: p) as checked:
                reference = "a/./b/../table.json"
                self.assertEqual(resolve_reference(root, reference, max_depth=2), root / "a/table.json")
                checked.assert_called_once()
                checked.reset_mock()
                for limit in (0, 1):
                    with self.assertRaisesRegex(ValueError, "depth budget"):
                        resolve_reference(root, reference, max_depth=limit)
                checked.assert_not_called()
                self.assertEqual(resolve_reference(root, "a/b/table.json"), root / "a/b/table.json")
                literal = r"a\b\table.json"
                depth = len(grammar(literal).parts)
                self.assertEqual(resolve_reference(root, literal, max_depth=depth), root / literal)
                with self.assertRaisesRegex(ValueError, "depth budget"):
                    resolve_reference(root, literal, max_depth=depth - 1)
                for refused in ROOTED_REFERENCES + ESCAPING_REFERENCES:
                    with self.subTest(reference=refused), self.assertRaisesRegex(ValueError, "fixture reference"):
                        resolve_reference(root, refused, max_depth=2)

    def test_budgeted_reference_checks_leave_and_reenter_links_and_selected_root_alias(self):
        with tempfile.TemporaryDirectory() as temp:
            parent = Path(temp)
            root, outside, alias = parent / "root", parent / "outside", parent / "alias"
            root.mkdir()
            outside.mkdir()
            write(root, "table.json", "{}")
            # Junctions cover Windows without requiring symlink privileges.
            with contextlib.ExitStack() as cleanup:
                for link, target in ((outside / "back", root), (root / "exit", outside), (alias, root)):
                    directory_link(link, target)
                    cleanup.callback(remove_directory_link, link)
                self.assertEqual(resolve_reference(alias, "table.json", max_depth=1),
                                 root.resolve() / "table.json")
                self.assertEqual(resolve_reference(alias, "missing.json", max_depth=1),
                                 root.resolve() / "missing.json")
                with self.assertRaisesRegex(ValueError, "resolves outside root"):
                    resolve_reference(alias, "exit/back/table.json", max_depth=3)

    def test_real_links_are_contained_at_every_component(self):
        with tempfile.TemporaryDirectory() as temp:
            parent = Path(temp)
            root, outside = parent / "root", parent / "root-sibling"
            root.mkdir()
            outside.mkdir()
            write(outside, "table.json", "{}")
            write(root, "common/biomes.json", "{}")
            symlink_or_skip(self, root / "file.json", outside / "table.json")
            symlink_or_skip(self, root / "directory", outside, directory=True)
            symlink_or_skip(self, root / "dangling.json", outside / "missing.json")
            symlink_or_skip(self, outside / "back", root / "common", directory=True)
            if os.name != "nt":
                # Backslashes name separate entries under POSIX native rules.
                symlink_or_skip(self, root / "directory\\table.json", outside / "table.json")
                symlink_or_skip(self, root / "directory\\back\\biomes.json", outside / "table.json")
            for reference in ("file.json", "directory/table.json", "directory\\table.json",
                              "directory/back/biomes.json", "directory\\back\\biomes.json",
                              "dangling.json"):
                with self.subTest(reference=reference):
                    with self.assertRaisesRegex(ValueError, "resolves outside root"):
                        resolve_reference(root, reference)
            symlink_or_skip(self, root / "valid", root / "common", directory=True)
            if os.name != "nt":
                symlink_or_skip(self, root / "valid\\biomes.json", root / "common/biomes.json")
            for reference in ("valid/biomes.json", "valid\\biomes.json"):
                self.assertEqual(resolve_reference(root, reference).read_text(), "{}")
            self.assertEqual(resolve_reference(root, "common/missing.json"),
                             root.resolve() / "common/missing.json")


def write(root: Path, relative: str, text: str) -> None:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


class FixtureHashTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name).resolve()

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_text_reader_matches_universal_newlines_and_encoding_errors(self):
        path = self.root / "text.json"
        for raw in (b"one\ntwo\nthree\n", b"one\r\ntwo\r\nthree\r\n",
                    b"one\rtwo\rthree\r", b"one\r\ntwo\rthree\n", b"", b"\xef\xbb\xbftext\r\n"):
            path.write_bytes(raw)
            for encoding in ("utf-8", "utf-8-sig"):
                with self.subTest(raw=raw, encoding=encoding):
                    self.assertEqual(fixture_paths.read_text(self.root, path, encoding=encoding),
                                     path.read_text(encoding=encoding))
                    self.assertEqual(fixture_paths.read_bytes(self.root, path), raw)
        for raw in (b"\xff", b"\xe2\x82"):
            path.write_bytes(raw)
            with self.subTest(raw=raw), self.assertRaises(UnicodeDecodeError):
                fixture_paths.read_text(self.root, path)

    def test_text_reader_enforces_byte_limit_before_decoding(self):
        path = self.root / "text.json"
        path.write_bytes(b"\xef\xbb\xbf\r\n")
        self.assertEqual(fixture_paths.read_text(self.root, path, encoding="utf-8-sig", limit=5), "\n")
        with self.assertRaisesRegex(ValueError, "oversized fixture"):
            fixture_paths.read_text(self.root, path, encoding="utf-8-sig", limit=4)

    def test_text_reader_refuses_growth_after_size_check(self):
        path = self.root / "text.json"
        path.write_bytes(b"text")
        original_fstat = os.fstat
        checked = 0

        def grow(descriptor):
            nonlocal checked
            status = original_fstat(descriptor)
            if stat.S_ISREG(status.st_mode):
                checked += 1
                if checked == 2:
                    with path.open("ab") as output:
                        output.write(b"\r\n")
            return status

        with mock.patch.object(os, "fstat", side_effect=grow), \
                self.assertRaisesRegex(ValueError, "oversized fixture"):
            fixture_paths.read_text(self.root, path, limit=4)
        self.assertEqual(checked, 2)

    @unittest.skipIf(os.name != "posix" or not hasattr(os, "geteuid") or os.geteuid() == 0,
                     "requires POSIX permission checks without root privileges")
    def test_readable_fixture_below_search_only_ancestor(self):
        ancestor = self.root / "shared"
        ancestor.mkdir()
        path = ancestor / "fixture.json"
        path.write_bytes(b"{}")
        # Reproduce a non-owner's access through a shared 0711 directory.
        ancestor.chmod(0o111)
        try:
            self.assertEqual(stat.S_IMODE(ancestor.stat().st_mode), 0o111)
            with self.assertRaises(PermissionError):
                list(ancestor.iterdir())
            self.assertEqual(path.read_bytes(), b"{}")
            self.assertEqual(fixture_paths.read_bytes(self.root, path), b"{}")
            ancestor.chmod(0o600)
            with self.assertRaises(PermissionError):
                fixture_paths.read_bytes(self.root, path)
        finally:
            ancestor.chmod(0o700)

    @unittest.skipUnless(os.name == "posix", "requires directory descriptor anchoring")
    def test_directory_flag_fallbacks_keep_anchoring_and_close_on_failure(self):
        path = self.root / "fixture.json"
        path.write_bytes(b"{}")
        original_open = os.open
        for mode in ("search", "read"):
            attributes = {key: value for key, value in vars(os).items()
                          if key not in ("O_PATH", "O_SEARCH")}
            if mode == "search":
                # Exercise selection on hosts that expose only O_SEARCH.
                attributes["O_SEARCH"] = os.O_RDONLY
            descriptors = []

            def anchored_open(name, flags, *args, **kwargs):
                self.assertTrue(flags & os.O_NOFOLLOW)
                if name in (path.name, path):
                    self.assertIn(kwargs["dir_fd"], descriptors)
                    raise PermissionError("fixture is unreadable")
                self.assertTrue(flags & os.O_DIRECTORY)
                descriptor = original_open(name, flags, *args, **kwargs)
                descriptors.append(descriptor)
                return descriptor

            with self.subTest(mode=mode), \
                    mock.patch.object(fixture_paths, "os", types.SimpleNamespace(**attributes)):
                self.assertEqual(fixture_paths.read_bytes(self.root, path), b"{}")
            with self.subTest(mode=mode), \
                    mock.patch.object(fixture_paths, "os", types.SimpleNamespace(**attributes)), \
                    mock.patch.object(fixture_paths.os, "open", side_effect=anchored_open), \
                    self.assertRaises(PermissionError):
                fixture_paths.read_bytes(self.root, path)
            for descriptor in descriptors:
                with self.assertRaises(OSError):
                    os.fstat(descriptor)

    def test_missing_optional_placement_matches_contained_dangling_link(self):
        # The native loader uses compiled defaults for both forms of absence.
        common = self.root / "common"
        write(common, "structures/cairn/cairn.json", '{"weight":1}')
        placement = common / "structures/cairn" / ("PLACEMENT.JSON" if os.name == "nt" else "placement.json")
        roots = (common, self.root)
        before = {(str(root), scope): module.fixture_hash(root, scope=scope)
                  for root in roots for scope in module.SCOPES}
        # A junction exercises the same dangling status without Windows symlink
        # privilege; on POSIX this is a real symbolic link.
        directory_link(placement, placement.parent / "missing.json")
        try:
            self.assertFalse(placement.exists())
            for root in roots:
                for scope in module.SCOPES:
                    with self.subTest(root=root, scope=scope):
                        self.assertEqual(module.fixture_hash(root, scope=scope),
                                         before[str(root), scope])
        finally:
            remove_directory_link(placement)
        self.assertTrue((common / "structures/cairn/cairn.json").is_file())
        placement.write_text('{"spacing":96}')
        for root in roots:
            for scope in module.SCOPES:
                self.assertNotEqual(module.fixture_hash(root, scope=scope),
                                    before[str(root), scope])

    def test_missing_optional_placement_uses_native_filename_case_rules(self):
        for grammar in (PurePosixPath, PureWindowsPath):
            root = grammar("data")
            for name, optional in (("structures/cairn/placement.json", True),
                                   ("common/structures/cairn/placement.json", True),
                                   ("COMMON/STRUCTURES/cairn/PLACEMENT.JSON", grammar is PureWindowsPath),
                                   ("structures/cairn/cairn.json", False),
                                   ("structures/cairn/nested/placement.json", False)):
                contained = mock.Mock()
                contained.stat.side_effect = FileNotFoundError("absent")
                with self.subTest(grammar=grammar.__name__, name=name), \
                        mock.patch.object(module, "Path", grammar), \
                        mock.patch.object(module, "contain_path", return_value=contained) as checked:
                    if optional:
                        self.assertEqual(module.fixture_entry_status(root, root / name), (contained, None))
                    else:
                        with self.assertRaises(FileNotFoundError):
                            module.fixture_entry_status(root, root / name)
                    checked.assert_called_once_with(root, root / name)

    def test_missing_tree_entries_other_than_optional_placement_are_refused(self):
        for name in ("structures/cairn/cairn.json", "structures/cairn/nested/placement.json",
                     "placement.json"):
            link = self.root / name
            link.parent.mkdir(parents=True, exist_ok=True)
            directory_link(link, self.root / "absent.json")
            try:
                for scope in module.SCOPES:
                    with self.subTest(name=name, scope=scope), self.assertRaises(FileNotFoundError):
                        module.fixture_hash(self.root, scope=scope)
            finally:
                remove_directory_link(link)

    def test_optional_placement_still_refuses_outside_and_cyclic_links(self):
        placement = self.root / "structures/cairn/placement.json"
        placement.parent.mkdir(parents=True)
        with tempfile.TemporaryDirectory() as temporary:
            outside = Path(temporary)
            for target in (outside, outside / "missing.json", placement.parent):
                directory_link(placement, target)
                try:
                    for scope in module.SCOPES:
                        with self.subTest(target=target, scope=scope), self.assertRaises(ValueError):
                            module.fixture_hash(self.root, scope=scope)
                finally:
                    remove_directory_link(placement)

    def test_optional_placement_stat_errors_and_nonregular_metadata_are_refused(self):
        placement = self.root / "structures/cairn/placement.json"
        write(self.root, "structures/cairn/placement.json", "{}")
        original_stat = Path.stat
        for failure in (PermissionError(errno.EACCES, "unreadable"),
                        OSError(errno.EIO, "partial status"),
                        NotADirectoryError(errno.ENOTDIR, "not a directory"),
                        os.stat_result((0,) + (0,) * 9),
                        os.stat_result((stat.S_IFIFO,) + (0,) * 9),
                        os.stat_result((stat.S_IFSOCK,) + (0,) * 9)):
            def status(path, *args, **kwargs):
                if path == placement:
                    if isinstance(failure, OSError):
                        raise failure
                    return failure
                return original_stat(path, *args, **kwargs)

            with self.subTest(failure=failure), mock.patch.object(Path, "stat", status), \
                    self.assertRaises((OSError, ValueError)):
                module.fixture_hash(self.root)

    def test_optional_placement_existing_directory_and_fifo_are_refused(self):
        placement = self.root / "structures/cairn/placement.json"
        placement.mkdir(parents=True)
        with self.assertRaisesRegex(ValueError, "not a regular file"):
            module.fixture_hash(self.root)
        placement.rmdir()
        if hasattr(os, "mkfifo"):
            os.mkfifo(placement)
            try:
                with self.assertRaisesRegex(ValueError, "not a regular file"):
                    module.fixture_hash(self.root)
            finally:
                placement.unlink()
    def test_directory_alias_adds_a_logical_pool_to_hash(self):
        write(self.root, "structures/template/pool.json", '{"weight":1}')
        before = module.fixture_hash(self.root)
        link = self.root / "structures/cairn"
        directory_link(link, self.root / "structures/template")
        try:
            linked = module.fixture_hash(self.root)
            self.assertNotEqual(before, linked)
        finally:
            remove_directory_link(link)
        write(self.root, "structures/cairn/pool.json", '{"weight":1}')
        self.assertEqual(linked, module.fixture_hash(self.root))

    def test_directory_alias_cycle_is_refused(self):
        write(self.root, "structures/pool.json", "{}")
        link = self.root / "structures/back"
        directory_link(link, self.root)
        try:
            with self.assertRaisesRegex(ValueError, "cyclic fixture directory"):
                module.fixture_hash(self.root)
        finally:
            remove_directory_link(link)

    def test_swapped_ancestor_cannot_redirect_fixture_reads(self):
        with tempfile.TemporaryDirectory() as temp:
            outside = Path(temp)
            write(self.root, "common/table.json", '{"inside":1}')
            write(outside, "table.json", '{"outside":1}')
            original_open = os.open
            original_read = io.open
            swapped = False

            def swap():
                nonlocal swapped
                if not swapped:
                    swapped = True
                    (self.root / "common").rename(self.root / "retained")
                    directory_link(self.root / "common", outside)

            def open_descriptor(path, flags, *args, **kwargs):
                if path in (self.root / "common/table.json", "table.json"):
                    swap()
                return original_open(path, flags, *args, **kwargs)

            def open_stream(path, *args, **kwargs):
                if path == self.root / "common/table.json":
                    swap()
                return original_read(path, *args, **kwargs)

            try:
                with mock.patch.object(os, "open", side_effect=open_descriptor), \
                        mock.patch.object(io, "open", side_effect=open_stream):
                    try:
                        result = module.canonical_file_digest(self.root / "common/table.json",
                                                              reference_root=self.root)
                    except (OSError, ValueError):
                        pass
                    else:
                        self.assertEqual(result, hashlib.sha256(b'{"inside":1}').hexdigest())
                self.assertTrue(swapped)
            finally:
                if swapped:
                    remove_directory_link(self.root / "common")

    def test_regular_file_replacement_before_open_is_refused(self):
        table = self.root / "table.json"
        write(self.root, table.name, "{}")
        replacement = self.root / "replacement.json"
        replacement.write_text('{"replacement":1}')
        original_open = os.open

        def replace(path, flags, *args, **kwargs):
            if path in (table, table.name):
                table.rename(self.root / "retained.json")
                replacement.rename(table)
            return original_open(path, flags, *args, **kwargs)

        with mock.patch.object(os, "open", side_effect=replace), \
                self.assertRaisesRegex(ValueError, "changed target"):
            module.canonical_file_digest(table)

    def test_windows_opened_path_is_checked_before_reading(self):
        table = self.root / "table.json"
        write(self.root, table.name, "{}")
        windows_os = types.SimpleNamespace(**(vars(os) | {"name": "nt"}))
        with mock.patch.object(fixture_paths, "os", windows_os, create=True), \
                mock.patch.object(fixture_paths, "_windows_final_path", create=True,
                                  return_value=self.root.parent / "outside.json"), \
                mock.patch.object(windows_os, "fdopen", side_effect=AssertionError("unchecked read")), \
                self.assertRaisesRegex(ValueError, "changed target"):
            module.canonical_file_digest(table)

    def test_selected_root_alias_does_not_bypass_non_regular_metadata(self):
        actual = self.root / "actual"
        actual.mkdir()
        (actual / "table.json").write_bytes(b"{}")
        alias = self.root / "alias"
        directory_link(alias, actual)
        path = alias / "table.json"
        original_stat = Path.stat
        try:
            for mode in (stat.S_IFIFO, stat.S_IFSOCK, stat.S_IFCHR, stat.S_IFBLK, stat.S_IFDIR):
                def status(name, *args, **kwargs):
                    if name == path:
                        return os.stat_result((mode,) + (0,) * 9)
                    return original_stat(name, *args, **kwargs)

                with self.subTest(mode=mode), mock.patch.object(Path, "stat", status), \
                        mock.patch.object(os, "open", side_effect=AssertionError("opened non-regular input")), \
                        self.assertRaisesRegex(ValueError, "not a regular file"):
                    fixture_paths.read_bytes(alias, path)
        finally:
            remove_directory_link(alias)

    @unittest.skipUnless(hasattr(os, "mkfifo"), "OS has no mkfifo")
    def test_fifo_tree_entry_is_refused(self):
        os.mkfifo(self.root / "pool.json")
        with self.assertRaisesRegex(ValueError, "not a regular file"):
            module.fixture_hash(self.root)

    def test_external_biome_references_are_refused_in_both_scopes(self):
        for scope in module.SCOPES:
            for reference in ROOTED_REFERENCES + ESCAPING_REFERENCES:
                with self.subTest(scope=scope, reference=reference):
                    write(self.root, "preset.json", json.dumps({
                        "generation_params": {"biomes": {"table": reference}}}))
                    with self.assertRaisesRegex(ValueError, "fixture reference"):
                        module.fixture_hash(self.root, scope=scope)

    def test_in_root_biome_reference_preserves_hash_format(self):
        for reference in ("common/./sub/../biomes.json", "common\\biomes.json", ""):
            write(self.root, "preset.json", json.dumps({
                "generation_params": {"biomes": {"table": reference}}}))
            write(self.root, "common/biomes.json", '{"biomes":[]}')
            write(self.root, "common\\biomes.json", '{"biomes":[]}')
            expected = hashlib.sha256()
            names = {"common/biomes.json", Path("common\\biomes.json").as_posix(), "preset.json"}
            for name in sorted(names):
                expected.update(name.encode() + b"\0")
                expected.update(module.canonical_file_digest(self.root / name).encode() + b"\n")
            self.assertEqual(module.fixture_hash(self.root), expected.hexdigest())

    def test_tree_hash_refuses_external_file_directory_and_dangling_links(self):
        with tempfile.TemporaryDirectory() as temp:
            outside = Path(temp)
            write(outside, "table.json", "{}")
            for target, directory in ((outside / "table.json", False), (outside, True),
                                      (outside / "missing.json", False)):
                link = self.root / "link"
                symlink_or_skip(self, link, target, directory=directory)
                try:
                    for scope in module.SCOPES:
                        with self.assertRaisesRegex(ValueError, "resolves outside root"):
                            module.fixture_hash(self.root, scope=scope)
                finally:
                    link.unlink()

    def test_in_root_file_link_hashes_with_its_original_logical_name(self):
        write(self.root, "table.json", "{}")
        symlink_or_skip(self, self.root / "alias.json", self.root / "table.json")
        linked = module.fixture_hash(self.root)
        (self.root / "alias.json").unlink()
        write(self.root, "alias.json", "{}")
        self.assertEqual(module.fixture_hash(self.root), linked)

    def test_enumerated_native_backslash_filename_keeps_its_bytes(self):
        if os.name == "nt":
            # Windows has no literal backslash filename. The native spelling
            # case is covered below on every filesystem that supports it.
            self.assertEqual(Path("literal\\table.json").parts, ("literal", "table.json"))
            return
        write(self.root, "literal\\table.json", '{"literal":1}')
        write(self.root, "literal/table.json", '{"nested":2}')
        expected = hashlib.sha256()
        for name in ("literal/table.json", "literal\\table.json"):
            expected.update(name.encode() + b"\0")
            expected.update(module.canonical_file_digest(self.root / name).encode() + b"\n")
        before = module.fixture_hash(self.root)
        self.assertEqual(before, expected.hexdigest())
        write(self.root, "literal\\table.json", '{"literal":3}')
        self.assertNotEqual(before, module.fixture_hash(self.root))

    def test_dependencies_outside_tree_are_hashed_with_stable_framing(self):
        common = self.root / "common"
        for name, reference in (("Z", "extra/z.json"), ("a", "extra/a.json"),
                                ("alias", "extra\\a.json")):
            write(common, name + ".json", json.dumps({
                "generation_params": {"biomes": {"table": reference}}}))
        write(self.root, "extra/a.json", '{"value":1}')
        if os.name != "nt":
            write(self.root, "extra\\a.json", '{"value":4}')
        write(self.root, "extra/z.json", '{"value":2}')
        expected = hashlib.sha256()
        for name in ("Z.json", "a.json", "alias.json"):
            expected.update(name.encode() + b"\0")
            expected.update(module.canonical_file_digest(common / name).encode() + b"\n")
        expected.update(b"\0fixture-dependencies:v1\0")
        names = {"extra/a.json", "extra/z.json", Path("extra\\a.json").as_posix()}
        entries = [(name, module.canonical_file_digest(self.root / name)) for name in sorted(names)]
        expected.update(json.dumps(entries, ensure_ascii=True, separators=(",", ":")).encode("ascii"))
        before = module.fixture_hash(common, reference_root=self.root)
        self.assertEqual(before, expected.hexdigest())
        write(self.root, "extra/a.json", '{"value":3}')
        self.assertNotEqual(before, module.fixture_hash(common, reference_root=self.root))
        (self.root / "extra/a.json").unlink()
        missing = module.fixture_hash(common, reference_root=self.root)
        self.assertNotEqual(before, missing)
        write(self.root, "extra/a.json", '{"value":1}')
        self.assertEqual(before, module.fixture_hash(common, reference_root=self.root))

    def test_authored_backslash_dependency_hashes_the_native_file(self):
        presets = self.root / "presets"
        reference = "extra\\biomes.json"
        write(presets, "preset.json", json.dumps({
            "generation_params": {"biomes": {"table": reference}}}))
        write(self.root, "extra/biomes.json", '{"value":1}')
        if os.name != "nt":
            write(self.root, reference, '{"value":2}')
        native = self.root.resolve() / reference
        self.assertEqual(resolve_reference(self.root, reference), native)
        dependencies = {}
        before = module.fixture_hash(presets, reference_root=self.root, dependencies=dependencies)
        self.assertEqual(dependencies, {Path(reference).as_posix(): native})
        write(self.root, reference, '{"value":3}')
        after = module.fixture_hash(presets, reference_root=self.root)
        self.assertNotEqual(before, after)
        if os.name != "nt":
            # The slash spelling is a different, unreferenced file on POSIX.
            write(self.root, "extra/biomes.json", '{"value":4}')
            self.assertEqual(after, module.fixture_hash(presets, reference_root=self.root))

    def test_non_regular_dependencies_are_refused_before_opening(self):
        presets = self.root / "presets"
        write(presets, "preset.json", json.dumps({
            "generation_params": {"biomes": {"table": "extra/biomes.json"}}}))
        table = self.root.resolve() / "extra/biomes.json"
        table.mkdir(parents=True)
        with mock.patch.object(os, "open", wraps=os.open) as opened:
            with self.assertRaisesRegex(ValueError, "fixture reference is not a regular file"):
                module.fixture_hash(presets, reference_root=self.root)
        self.assertNotIn(table, [call.args[0] for call in opened.call_args_list])
        # Exercise every non-regular mode on all hosts without creating devices.
        for mode in (stat.S_IFIFO, stat.S_IFSOCK, stat.S_IFCHR, stat.S_IFBLK, stat.S_IFDIR):
            with self.subTest(mode=mode), \
                    mock.patch.object(Path, "stat", return_value=os.stat_result((mode,) + (0,) * 9)), \
                    mock.patch.object(os, "open", side_effect=AssertionError("opened non-regular file")), \
                    self.assertRaisesRegex(ValueError, "fixture reference is not a regular file"):
                module.canonical_file_digest(table)

    def test_opened_non_regular_descriptors_are_closed_without_reading(self):
        write(self.root, "table.json", "{}")
        for mode in (stat.S_IFIFO, stat.S_IFSOCK, stat.S_IFCHR, stat.S_IFBLK, stat.S_IFDIR):
            with self.subTest(mode=mode), \
                    mock.patch.object(os, "fstat", return_value=os.stat_result((mode,) + (0,) * 9)), \
                    mock.patch.object(os, "fdopen", side_effect=AssertionError("read non-regular file")), \
                    mock.patch.object(os, "open", wraps=os.open) as opened, \
                    mock.patch.object(os, "close", wraps=os.close) as closed, \
                    self.assertRaisesRegex(ValueError, "fixture reference is not a regular file"):
                module.canonical_file_digest(self.root / "table.json")
            self.assertEqual(opened.call_count, closed.call_count)
            file_opens = [call for call in opened.call_args_list
                          if call.args[0] in (self.root / "table.json", "table.json")]
            self.assertEqual(len(file_opens), 1)
            with self.assertRaises(OSError) as invalid:
                os.fstat(closed.call_args.args[0])
            self.assertEqual(invalid.exception.errno, errno.EBADF)

    @unittest.skipUnless(hasattr(os, "mkfifo"), "OS has no mkfifo")
    def test_fifo_dependency_and_swap_before_open_are_refused(self):
        presets = self.root / "presets"
        write(presets, "preset.json", json.dumps({
            "generation_params": {"biomes": {"table": "extra/biomes.json"}}}))
        table = self.root.resolve() / "extra/biomes.json"
        table.parent.mkdir()
        os.mkfifo(table)
        with self.assertRaisesRegex(ValueError, "fixture reference is not a regular file"):
            module.fixture_hash(presets, reference_root=self.root)
        table.unlink()
        table.write_text("{}", encoding="utf-8")
        original_open = os.open
        opened_descriptors = []

        def swap_before_open(path, flags, *args, **kwargs):
            if path in (table, table.name):
                table.unlink()
                os.mkfifo(table)
                self.assertTrue(flags & os.O_NONBLOCK)
                descriptor = original_open(path, flags, *args, **kwargs)
                opened_descriptors.append(descriptor)
                return descriptor
            return original_open(path, flags, *args, **kwargs)

        with mock.patch.object(os, "open", side_effect=swap_before_open), \
                self.assertRaisesRegex(ValueError, "fixture reference is not a regular file"):
            module.fixture_hash(presets, reference_root=self.root)
        self.assertEqual(len(opened_descriptors), 1)
        with self.assertRaises(OSError) as closed:
            os.fstat(opened_descriptors[0])
        self.assertEqual(closed.exception.errno, errno.EBADF)

    def test_transitive_dependencies_and_cycles_are_hashed(self):
        presets = self.root / "presets"
        document = lambda table: json.dumps({"generation_params": {"biomes": {"table": table}}})
        write(presets, "first.json", document("extra/second.json"))
        write(self.root, "extra/second.json", document("extra/third.json"))
        write(self.root, "extra/third.json", document("extra/second.json"))
        before = module.fixture_hash(presets, reference_root=self.root)
        write(self.root, "extra/third.json", document("extra/second.json")[:-1] + ',"value":1}')
        self.assertNotEqual(before, module.fixture_hash(presets, reference_root=self.root))

    def test_existing_fixture_tree_receipts_and_preset_dependency_extension(self):
        checkout = Path(__file__).resolve().parents[2]
        cases = (
            ("data", "all"), ("data/common", "all"),
            ("data/common", "headless-server"), ("worlds/atlas/presets", "all"),
        )
        for relative, scope in cases:
            with self.subTest(relative=relative, scope=scope):
                root = checkout / relative
                # Preserve the original receipt algorithm as an oracle. Raw
                # non-JSON text bytes depend on the checkout newline policy.
                expected = hashlib.sha256()
                if scope != "all":
                    expected.update(f"fixture-scope:{scope}\0".encode("ascii"))
                for path in sorted((p for p in root.rglob("*") if p.is_file()),
                                   key=lambda p: p.relative_to(root).parts):
                    name = path.relative_to(root).as_posix()
                    if scope == "headless-server" and name in module.SERVER_RENDER_ONLY_FILES:
                        continue
                    expected.update(name.encode("utf-8") + b"\0")
                    expected.update(module.canonical_file_digest(path).encode("ascii") + b"\n")
                if relative == "worlds/atlas/presets":
                    # Presets reference common data outside their hashed tree.
                    # Their identity must now bind those bytes too.
                    expected.update(b"\0fixture-dependencies:v1\0")
                    entries = [("common/biomes.json", module.canonical_file_digest(
                        checkout / "data/common/biomes.json"))]
                    expected.update(json.dumps(entries, ensure_ascii=True,
                                               separators=(",", ":")).encode("ascii"))
                self.assertEqual(module.fixture_hash(checkout / relative, scope=scope,
                                                    reference_root=checkout / "data"), expected.hexdigest())

    def test_cli_reports_containment_refusal(self):
        write(self.root, "preset.json", json.dumps({
            "generation_params": {"biomes": {"table": "../outside.json"}}}))
        with contextlib.redirect_stderr(io.StringIO()) as error:
            self.assertEqual(module.main(["fixture_hash.py", str(self.root)]), 2)
        self.assertIn("fixture refused: fixture reference escapes root", error.getvalue())

    def test_comment_key_edit_does_not_move_the_hash(self) -> None:
        # The case that made a documentation-only change unable to pass the gate.
        write(self.root, "common/systems.json", json.dumps({"_note": "EXPERIMENTAL", "a": 1}))
        before = module.fixture_hash(self.root)
        write(self.root, "common/systems.json", json.dumps({"_note": "supported now", "a": 1}))
        self.assertEqual(before, module.fixture_hash(self.root))

    def test_adding_or_removing_a_comment_key_does_not_move_the_hash(self) -> None:
        write(self.root, "a.json", json.dumps({"a": 1}))
        before = module.fixture_hash(self.root)
        write(self.root, "a.json", json.dumps({"a": 1, "_why": "explanation"}))
        self.assertEqual(before, module.fixture_hash(self.root))

    def test_nested_comment_keys_are_stripped(self) -> None:
        write(self.root, "a.json", json.dumps({"o": {"_c": "x", "v": 1}, "l": [{"_c": "y", "v": 2}]}))
        before = module.fixture_hash(self.root)
        write(self.root, "a.json", json.dumps({"o": {"_c": "CHANGED", "v": 1}, "l": [{"_c": "Z", "v": 2}]}))
        self.assertEqual(before, module.fixture_hash(self.root))

    def test_reindentation_and_key_order_do_not_move_the_hash(self) -> None:
        write(self.root, "a.json", '{"b":2,"a":1}')
        before = module.fixture_hash(self.root)
        write(self.root, "a.json", '{\n  "a": 1,\n  "b": 2\n}\n')
        self.assertEqual(before, module.fixture_hash(self.root))

    def test_a_real_value_change_moves_the_hash(self) -> None:
        # The property the gate actually depends on.
        write(self.root, "a.json", json.dumps({"a": 1}))
        before = module.fixture_hash(self.root)
        write(self.root, "a.json", json.dumps({"a": 2}))
        self.assertNotEqual(before, module.fixture_hash(self.root))

    def test_a_non_json_change_moves_the_hash(self) -> None:
        (self.root / "t.ltex").write_bytes(b"\x01\x02")
        before = module.fixture_hash(self.root)
        (self.root / "t.ltex").write_bytes(b"\x01\x03")
        self.assertNotEqual(before, module.fixture_hash(self.root))

    def test_adding_and_renaming_files_move_the_hash(self) -> None:
        write(self.root, "a.json", json.dumps({"a": 1}))
        before = module.fixture_hash(self.root)
        write(self.root, "b.json", json.dumps({"b": 1}))
        added = module.fixture_hash(self.root)
        self.assertNotEqual(before, added)
        (self.root / "b.json").rename(self.root / "c.json")
        self.assertNotEqual(added, module.fixture_hash(self.root))

    def test_malformed_json_is_hashed_raw_and_still_registers(self) -> None:
        write(self.root, "a.json", "{not valid json")
        before = module.fixture_hash(self.root)
        write(self.root, "a.json", "{also not valid")
        self.assertNotEqual(before, module.fixture_hash(self.root))

    def test_byte_order_mark_is_tolerated(self) -> None:
        # Several fixtures under data/ carry a BOM.
        (self.root / "a.json").write_bytes(b"\xef\xbb\xbf" + json.dumps({"a": 1}).encode("utf-8"))
        with_bom = module.fixture_hash(self.root)
        (self.root / "a.json").write_bytes(json.dumps({"a": 1}).encode("utf-8"))
        self.assertEqual(with_bom, module.fixture_hash(self.root))

    def test_hash_is_stable_across_calls_and_shaped_for_the_validator(self) -> None:
        write(self.root, "a.json", json.dumps({"a": 1}))
        first = module.fixture_hash(self.root)
        self.assertEqual(first, module.fixture_hash(self.root))
        # perf.py requires [0-9a-f]{40,64} for a fixture hash.
        self.assertEqual(len(first), 64)
        self.assertTrue(all(c in "0123456789abcdef" for c in first))

    def test_path_order_is_case_sensitive_and_preserves_component_order(self) -> None:
        (self.root / "a.bin").write_bytes(b"lower")
        (self.root / "Z.bin").write_bytes(b"upper")
        (self.root / "a").mkdir()
        (self.root / "a/child.bin").write_bytes(b"nested")
        expected = hashlib.sha256()
        for name, payload in (("Z.bin", b"upper"), ("a/child.bin", b"nested"), ("a.bin", b"lower")):
            expected.update(name.encode("utf-8") + b"\0")
            expected.update(hashlib.sha256(payload).hexdigest().encode("ascii") + b"\n")
        self.assertEqual(module.fixture_hash(self.root), expected.hexdigest())

    def test_server_scope_omits_scatter_but_default_detects_its_values(self) -> None:
        write(self.root, "systems.json", '{"water":true}')
        write(self.root, "foliage/scatter_set.json", '{"height":0.44}')
        full = module.fixture_hash(self.root)
        server = module.fixture_hash(self.root, scope="headless-server")
        write(self.root, "foliage/scatter_set.json", '{"height":0.20}')
        self.assertNotEqual(full, module.fixture_hash(self.root))
        self.assertEqual(server, module.fixture_hash(self.root, scope="headless-server"))

    def test_server_scope_omits_only_the_named_precipitation_descriptors(self) -> None:
        for relative in ("particles/precip_rain.json", "particles/precip_snow.json"):
            write(self.root, relative, '{"size":0.1}')
            full = module.fixture_hash(self.root)
            server = module.fixture_hash(self.root, scope="headless-server")
            write(self.root, relative, '{"size":0.02}')
            self.assertNotEqual(full, module.fixture_hash(self.root))
            self.assertEqual(server, module.fixture_hash(self.root, scope="headless-server"))
        before = module.fixture_hash(self.root, scope="headless-server")
        write(self.root, "particles/another.json", '{"size":0.02}')
        self.assertNotEqual(before, module.fixture_hash(self.root, scope="headless-server"))

    def test_server_scope_keeps_simulation_and_other_foliage_changes(self) -> None:
        for relative in ("systems.json", "foliage/species/wheat.json", "foliage/new_species.json"):
            with self.subTest(relative=relative):
                write(self.root, relative, '{"growth_rate":1}')
                before = module.fixture_hash(self.root, scope="headless-server")
                write(self.root, relative, '{"growth_rate":2}')
                self.assertNotEqual(before, module.fixture_hash(self.root, scope="headless-server"))
                before = module.fixture_hash(self.root, scope="headless-server")
                write(self.root, relative, '{malformed')
                self.assertNotEqual(before, module.fixture_hash(self.root, scope="headless-server"))

    def test_server_scope_still_detects_added_renamed_and_removed_files(self) -> None:
        before = module.fixture_hash(self.root, scope="headless-server")
        write(self.root, "foliage/new.json", '{"growth_rate":1}')
        added = module.fixture_hash(self.root, scope="headless-server")
        self.assertNotEqual(before, added)
        (self.root / "foliage/new.json").rename(self.root / "foliage/renamed.json")
        renamed = module.fixture_hash(self.root, scope="headless-server")
        self.assertNotEqual(added, renamed)
        (self.root / "foliage/renamed.json").unlink()
        self.assertEqual(before, module.fixture_hash(self.root, scope="headless-server"))

    def test_scopes_have_distinct_identities_even_without_scatter_file(self) -> None:
        write(self.root, "systems.json", '{"water":true}')
        self.assertNotEqual(
            module.fixture_hash(self.root),
            module.fixture_hash(self.root, scope="headless-server"),
        )

    def test_cli_passes_scope_and_preserves_default(self) -> None:
        write(self.root, "systems.json", '{"water":true}')
        for scope_args, expected_scope in (([], "all"), (["--scope", "headless-server"], "headless-server")):
            with self.subTest(scope=expected_scope), contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(module.main(["fixture_hash.py", str(self.root), *scope_args]), 0)
                self.assertEqual(output.getvalue().strip(), module.fixture_hash(self.root, scope=expected_scope))

    def test_unknown_scope_is_rejected(self) -> None:
        with self.assertRaises(ValueError):
            module.fixture_hash(self.root, scope="typo")
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
            module.main(["fixture_hash.py", str(self.root), "--scope", "typo"])
        self.assertEqual(raised.exception.code, 2)


class FixtureTraversalTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()

    def fanout(self, levels=3):
        # A small real acyclic graph, also supported without Windows symlink
        # privilege. Explicit reverse cleanup never traverses a junction target.
        for level in range(levels + 1):
            (self.root / str(level)).mkdir()
        for level in range(levels):
            for name in ("left", "right"):
                link = self.root / str(level) / name
                directory_link(link, self.root / str(level + 1))
                self.addCleanup(remove_directory_link, link)
        (self.root / str(levels) / "leaf.bin").write_bytes(b"leaf")
        return self.root

    def test_small_acyclic_fanout_bounds_expanded_work_before_hash_reads(self):
        root = self.fanout()
        # 27 logical directories (including root), 15 logical files.
        with mock.patch.object(module, "MAX_FIXTURE_ENTRIES", 42, create=True):
            admitted = module.fixture_hash(root, reference_root=self.root)
        with mock.patch.object(module, "MAX_FIXTURE_ENTRIES", 41, create=True), \
                mock.patch.object(module, "canonical_file_digest",
                                  side_effect=AssertionError("read before preflight")), \
                self.assertRaisesRegex(ValueError, "entry budget"):
            module.fixture_hash(root, reference_root=self.root)
        names = [p.relative_to(root).as_posix() for p, _ in module.fixture_files(root)]
        expected = hashlib.sha256()
        for name in sorted(names):
            expected.update(name.encode() + b"\0" + hashlib.sha256(b"leaf").hexdigest().encode() + b"\n")
        self.assertEqual(len(names), 15)
        self.assertEqual(len(set(names)), 15)
        self.assertEqual(admitted, expected.hexdigest())
        self.assertEqual(admitted, module.fixture_hash(root, reference_root=self.root))

    def test_empty_directories_are_charged_including_root(self):
        for name in ("a", "b", "c", "d", "e"):
            (self.root / name).mkdir()
        with mock.patch.object(module, "MAX_FIXTURE_ENTRIES", 6, create=True):
            self.assertEqual(module.fixture_hash(self.root), hashlib.sha256().hexdigest())
        with mock.patch.object(module, "MAX_FIXTURE_ENTRIES", 5, create=True), \
                self.assertRaisesRegex(ValueError, "entry budget"):
            module.fixture_hash(self.root)
        with mock.patch.object(module, "MAX_FIXTURE_ENTRIES", 0, create=True), \
                self.assertRaisesRegex(ValueError, "entry budget"):
            module.fixture_hash(self.root)

    def test_missing_optional_placement_entries_are_charged(self):
        structures = self.root / "structures"
        structures.mkdir()
        # Disappearing optional directory entries model the permitted missing
        # placement fallback on both hosts, without requiring file symlinks.
        for name in ("a", "b", "c"):
            folder = structures / name
            folder.mkdir()
            (folder / "placement.json").write_text("{}")
        original = Path.stat
        def missing(path, *args, **kwargs):
            if path.name == "placement.json":
                raise FileNotFoundError(str(path))
            return original(path, *args, **kwargs)
        with mock.patch.object(Path, "stat", missing):
            with mock.patch.object(module, "MAX_FIXTURE_ENTRIES", 8, create=True):
                self.assertEqual(module.fixture_hash(self.root), hashlib.sha256().hexdigest())
            with mock.patch.object(module, "MAX_FIXTURE_ENTRIES", 7, create=True), \
                    self.assertRaisesRegex(ValueError, "entry budget"):
                module.fixture_hash(self.root)

    def test_scope_exclusions_are_charged_before_filtering(self):
        for name in module.SERVER_RENDER_ONLY_FILES:
            path = self.root / name
            path.parent.mkdir(exist_ok=True)
            path.write_text("{}")
        with mock.patch.object(module, "MAX_FIXTURE_ENTRIES", 6, create=True):
            admitted = module.fixture_hash(self.root, scope="headless-server")
        self.assertEqual(admitted, hashlib.sha256(b"fixture-scope:headless-server\0").hexdigest())
        with mock.patch.object(module, "MAX_FIXTURE_ENTRIES", 5, create=True), \
                self.assertRaisesRegex(ValueError, "entry budget"):
            module.fixture_hash(self.root, scope="headless-server")

    def test_depth_boundary_and_scanners_close_on_refusal(self):
        directory = self.root / "a" / "b" / "c"
        directory.mkdir(parents=True)
        (directory / "leaf.bin").write_bytes(b"leaf")
        original = os.scandir
        opened = []
        def scan(path):
            scanner = original(path)
            opened.append(scanner)
            return scanner
        with mock.patch.object(module, "MAX_FIXTURE_DEPTH", 4, create=True):
            admitted = module.fixture_hash(self.root)
        with mock.patch.object(os, "scandir", side_effect=scan):
            with mock.patch.object(module, "MAX_FIXTURE_DEPTH", 3, create=True), \
                    self.assertRaisesRegex(ValueError, "depth budget"):
                module.fixture_hash(self.root)
            self.assertTrue(opened)
            self.assertTrue(all(next(scanner, None) is None for scanner in opened))
        self.assertEqual(admitted, module.fixture_hash(self.root))

    def test_streaming_listing_refuses_before_inspecting_or_retaining_overflow(self):
        for i in range(12):
            (self.root / str(i)).write_text("{}")
        original = module.fixture_entry_status
        with mock.patch.object(module, "MAX_FIXTURE_ENTRIES", 4, create=True), \
                mock.patch.object(module, "fixture_entry_status", wraps=original) as inspected, \
                mock.patch.object(os, "listdir", side_effect=AssertionError("eager directory listing")), \
                self.assertRaisesRegex(ValueError, "entry budget"):
            module.fixture_hash(self.root)
        self.assertEqual(inspected.call_count, 3)

    def test_scanner_open_and_iteration_errors_propagate_and_close_ancestors(self):
        (self.root / "child").mkdir()
        original = os.scandir
        for error in (PermissionError("denied"), OSError(errno.EIO, "scan failed")):
            opened = []
            def scan(path):
                if Path(path).name == "child":
                    raise error
                scanner = original(path)
                opened.append(scanner)
                return scanner
            with self.subTest(error=error), mock.patch.object(os, "scandir", side_effect=scan), \
                    self.assertRaises(type(error)):
                module.fixture_hash(self.root)
            self.assertTrue(all(next(scanner, None) is None for scanner in opened))

    def test_partial_scan_errors_and_abandoned_file_iterator_close_scanners(self):
        (self.root / "leaf.bin").write_bytes(b"leaf")
        original = os.scandir
        opened = []
        def scan(path):
            scanner = original(path)
            opened.append(scanner)
            return scanner
        with mock.patch.object(os, "scandir", side_effect=scan):
            files = module.fixture_files(self.root)
            self.assertEqual(next(files)[0], self.root / "leaf.bin")
            files.close()
        self.assertTrue(opened)
        self.assertTrue(all(next(scanner, None) is None for scanner in opened))
        class PartialScan:
            def __init__(self, path):
                self.scanner = original(path)
                self.calls = 0
                self.closed = False
            def __next__(self):
                self.calls += 1
                if self.calls == 2:
                    raise OSError(errno.EIO, "partial listing")
                return next(self.scanner)
            def close(self):
                self.closed = True
                self.scanner.close()
        scanner = PartialScan(self.root)
        with mock.patch.object(os, "scandir", return_value=scanner), \
                self.assertRaisesRegex(OSError, "partial listing"):
            module.fixture_hash(self.root)
        self.assertTrue(scanner.closed)

    def test_transitive_dependency_work_shares_tree_budget_and_preserves_output(self):
        presets = self.root / "presets"
        presets.mkdir()
        document = lambda name: json.dumps({"generation_params": {"biomes": {"table": name}}})
        (presets / "one.json").write_text(document("a.json"))
        (self.root / "a.json").write_text(document("b.json"))
        (self.root / "b.json").write_text(document("missing.json"))
        dependencies = {"sentinel": self.root / "sentinel"}
        with mock.patch.object(module, "MAX_FIXTURE_ENTRIES", 5, create=True):
            admitted = module.fixture_hash(presets, reference_root=self.root)
        with mock.patch.object(module, "MAX_FIXTURE_ENTRIES", 4, create=True), \
                self.assertRaisesRegex(ValueError, "entry budget"):
            module.fixture_hash(presets, reference_root=self.root, dependencies=dependencies)
        self.assertEqual(dependencies, {"sentinel": self.root / "sentinel"})
        self.assertEqual(admitted, module.fixture_hash(presets, reference_root=self.root))

    def test_deep_authored_references_refuse_before_containment_without_directories(self):
        presets = self.root / "presets"
        presets.mkdir()
        document = lambda name: json.dumps({"generation_params": {"biomes": {"table": name}}})
        reference = "a/" * 20000 + "missing.json"
        original = fixture_paths.contain_path

        def contain(root, path):
            # Count the actual input before any ancestor expansion or I/O. This
            # tripwire makes the unfixed test fail safely without allocating it.
            self.assertLessEqual(len(path.relative_to(root).parts), module.MAX_FIXTURE_DEPTH,
                                 "over-depth reference reached containment")
            return original(root, path)

        for transitive in (False, True):
            with self.subTest(transitive=transitive):
                (presets / "one.json").write_text(document("next.json" if transitive else reference))
                (self.root / "next.json").write_text(document(reference))
                dependencies = {"sentinel": self.root / "sentinel"}
                with mock.patch.object(fixture_paths, "contain_path", contain), \
                        self.assertRaisesRegex(ValueError, "depth budget"):
                    module.fixture_hash(presets, reference_root=self.root, dependencies=dependencies)
                self.assertEqual(dependencies, {"sentinel": self.root / "sentinel"})
                self.assertFalse((self.root / "a").exists())

    def test_missing_reference_at_depth_boundary_preserves_receipt_and_direct_helper(self):
        presets = self.root / "presets"
        presets.mkdir()
        name = "/".join(["a"] * (module.MAX_FIXTURE_DEPTH - 1) + ["missing.json"])
        document = {"generation_params": {"biomes": {"table": "drop/../" + name}}}
        preset = presets / "one.json"
        preset.write_text(json.dumps(document))
        canonical = json.dumps(document, sort_keys=True, separators=(",", ":"))
        file_digest = hashlib.sha256(canonical.encode()).hexdigest()
        expected = hashlib.sha256(b"one.json\0" + file_digest.encode() + b"\n")
        expected.update(b"\0fixture-dependencies:v1\0")
        expected.update(json.dumps([(name, None)], ensure_ascii=True, separators=(",", ":")).encode("ascii"))
        dependencies = {}
        self.assertEqual(module.fixture_hash(presets, reference_root=self.root, dependencies=dependencies),
                         expected.hexdigest())
        self.assertEqual(dependencies, {name: self.root / name})
        # The standalone digest has no traversal budget; preserve that contract.
        document["generation_params"]["biomes"]["table"] = "a/" + name
        preset.write_text(json.dumps(document))
        direct = {}
        module.canonical_file_digest(preset, reference_root=self.root, dependencies=direct)
        self.assertEqual(direct, {"a/" + name: self.root / ("a/" + name)})
        with self.assertRaisesRegex(ValueError, "depth budget"):
            module.fixture_hash(presets, reference_root=self.root)

    def test_dependency_depth_is_checked_and_cycles_still_terminate(self):
        presets = self.root / "presets"
        presets.mkdir()
        document = lambda name: json.dumps({"generation_params": {"biomes": {"table": name}}})
        (presets / "one.json").write_text(document("a/b/leaf.json"))
        leaf = self.root / "a/b/leaf.json"
        leaf.parent.mkdir(parents=True)
        leaf.write_text(document("a/b/leaf.json"))
        with mock.patch.object(module, "MAX_FIXTURE_DEPTH", 3, create=True):
            admitted = module.fixture_hash(presets, reference_root=self.root)
        with mock.patch.object(module, "MAX_FIXTURE_DEPTH", 2, create=True), \
                self.assertRaisesRegex(ValueError, "depth budget"):
            module.fixture_hash(presets, reference_root=self.root)
        self.assertEqual(admitted, module.fixture_hash(presets, reference_root=self.root))


class FixtureEvidenceReaderTests(unittest.TestCase):
    def setUp(self):
        import check_residency
        import perf
        import render_contract
        import validate_render_capture
        from test_perf import run_document, write_build_manifest
        from test_render_contract import artifact, args, QUALIFIED

        self.refusals = (OSError, ValueError, check_residency.InvalidInput)
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        # Hook the actual canonical open path, including Windows long names and
        # POSIX temporary directories reached through a linked ancestor.
        self.parent = Path(self.temporary.name).resolve()
        self.root = self.parent / "inputs"
        self.root.mkdir()
        build = self.parent / "build.json"
        write_build_manifest(build)
        text = (Path(__file__).parent / "fixtures/surface-flight.traversal").read_bytes().decode("utf-8")
        text = text.replace("duration_seconds 60", "duration_seconds 0.13333333333333333")
        text = text.replace("expected_ticks 1800", "expected_ticks 4")
        manifest = render_contract.traversal_manifest(text)
        traversal = artifact()
        traversal["workload"].update({key: value for key, value in manifest.items() if key != "path"})
        traversal["workload"].update(kind="traversal", actual_ticks=4, measured_duration_seconds=0.01,
                                     script=text, camera_samples=[render_contract.camera_sample(manifest, tick)
                                                                 for tick in range(1, 5)])

        def capture_cli(path):
            with mock.patch.object(sys, "argv", ["validate_render_capture.py", str(path),
                                                 "--qualified-renderer", QUALIFIED]), \
                    contextlib.redirect_stdout(io.StringIO()) as output:
                status = validate_render_capture.main()
            if status:
                raise ValueError(output.getvalue())
            self.assertEqual(json.loads(output.getvalue())["verdict"], "PASS")

        parameters = dict(ticks=2, surface_radius=1, collision_radius=1, seed=9, world_preset="default")
        smoke = dict(schema="luminumbra.server_tick.v1", passed=True, deterministic=True,
                     ticks_requested=2, surface_radius=1, collision_radius=1, seed="9", preset="default",
                     runs=[dict(ok=True, ticks_executed=2, chunks_streamed=1)] * 2,
                     world_hash="stable", world_hash_replay="stable")
        self.readers = [
            ("build manifest", perf.load_build_manifest, build.read_bytes()),
            ("server evidence", lambda p: perf.validate_server_smoke_evidence(p, parameters),
             json.dumps(smoke).encode()),
            ("evidence metric", lambda p: perf.read_evidence_metric(p, "water_phase_p95_ms"),
             b'{"water_phase_ms":{"total":{"p95":3.5}}}'),
            ("run", perf.load_run, json.dumps(run_document([1.0, 2.0, 3.0])).encode()),
            ("rebaseline", perf.load_rebaseline, json.dumps(dict(schema=perf.REBASELINE_SCHEMA,
                retired_comparability_key="a" * 64, replacement_comparability_key="b" * 64,
                reason="Fixture data changed")).encode()),
            ("residency profile", check_residency.load_profile,
             json.dumps(dict(schema=check_residency.PROFILE_SCHEMA, residency={})).encode()),
            ("residency artifact", check_residency.load_rows, b'{"residency":[]}'),
            ("traversal", lambda p: render_contract.validate(traversal, QUALIFIED, traversal=p), text.encode()),
            ("workload manifest", lambda p: validate_render_capture.validate(artifact(), args(workload_manifest=p)),
             b'{"seed":424242}'),
            ("render capture", capture_cli, json.dumps(artifact()).encode()),
        ]

    def check_replacement(self, reader, payload, replacement, *, before_read=False):
        path = self.root / "input.json"
        path.write_bytes(payload)
        reader(path)  # Every reader must accept the positive control first.
        outside = self.parent / "outside.json"
        outside.write_bytes(payload)
        if replacement == "symlink":
            probe = self.root / "probe"
            symlink_or_skip(self, probe, outside)
            probe.unlink()
        original_open, original_io_open = os.open, io.open
        swapped = False

        def swap():
            nonlocal swapped
            if not swapped:
                path.unlink()
                if replacement == "symlink":
                    path.symlink_to(outside)
                else:
                    os.mkfifo(path)
                swapped = True

        def open_descriptor(name, flags, *args, **kwargs):
            if name in (path, str(path), path.name):
                swap()
            return original_open(name, flags, *args, **kwargs)

        def open_stream(name, *args, **kwargs):
            if name in (path, str(path)):
                swap()
                if replacement == "fifo":
                    raise AssertionError("unchecked FIFO read")
            return original_io_open(name, *args, **kwargs)

        try:
            if before_read:
                swap()
            with mock.patch.object(os, "open", side_effect=open_descriptor), \
                    mock.patch.object(io, "open", side_effect=open_stream), \
                    mock.patch.object(os, "fdopen", side_effect=AssertionError("unchecked descriptor read")), \
                    self.assertRaises(self.refusals):
                reader(path)
            self.assertTrue(swapped)
        finally:
            path.unlink(missing_ok=True)

    def test_all_evidence_readers_refuse_symlink_swap_before_open(self):
        for name, reader, payload in self.readers:
            with self.subTest(reader=name):
                self.check_replacement(reader, payload, "symlink")

    def test_all_evidence_readers_verify_windows_handle_paths_before_reading(self):
        path = self.root / "input.json"
        windows_os = types.SimpleNamespace(**(vars(os) | {"name": "nt"}))
        for name, reader, payload in self.readers:
            path.write_bytes(payload)
            with self.subTest(reader=name, canonical=True), \
                    mock.patch.object(fixture_paths, "os", windows_os), \
                    mock.patch.object(fixture_paths, "_windows_final_path", return_value=path.resolve()):
                reader(path)
            for failure in (self.parent / "outside.json", self.root / "replacement.json",
                            OSError("handle path unavailable"),
                            ValueError("opened fixture path exceeds platform bound")):
                def final_path(descriptor):
                    self.assertTrue(stat.S_ISREG(os.fstat(descriptor).st_mode))
                    if isinstance(failure, Exception):
                        raise failure
                    return failure

                with self.subTest(reader=name, failure=str(failure)), \
                        mock.patch.object(fixture_paths, "os", windows_os), \
                        mock.patch.object(fixture_paths, "_windows_final_path", side_effect=final_path), \
                        mock.patch.object(windows_os, "fdopen", side_effect=AssertionError("unchecked read")), \
                        mock.patch.object(windows_os, "close", wraps=os.close) as closed, \
                        self.assertRaises(self.refusals):
                    reader(path)
                closed.assert_called_once()
                with self.assertRaises(OSError) as invalid:
                    os.fstat(closed.call_args.args[0])
                self.assertEqual(invalid.exception.errno, errno.EBADF)

    @unittest.skipUnless(hasattr(os, "mkfifo"), "OS has no mkfifo")
    def test_all_evidence_readers_refuse_fifo_before_and_during_open(self):
        for name, reader, payload in self.readers:
            for before_read in (False, True):
                with self.subTest(reader=name, before_read=before_read):
                    self.check_replacement(reader, payload, "fifo", before_read=before_read)

    def test_all_evidence_readers_refuse_empty_partial_and_non_utf8_data(self):
        path = self.root / "input.json"
        for name, reader, payload in self.readers:
            for invalid in (b"", b'{"schema":', b"\xe2\x82", b"\xff"):
                path.write_bytes(invalid)
                with self.subTest(reader=name, invalid=invalid), self.assertRaises(self.refusals):
                    reader(path)


if __name__ == "__main__":
    unittest.main()
