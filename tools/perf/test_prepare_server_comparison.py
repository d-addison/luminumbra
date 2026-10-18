#!/usr/bin/env python3
"""Controlled-workload identity and isolation contracts; stdlib only."""
import hashlib
import io
import json
import os
from pathlib import Path
import stat
import tempfile
import types
import unittest
from unittest import mock

import prepare_server_comparison as comparison
from test_fixture_hash import (ROOTED_REFERENCES, ESCAPING_REFERENCES, directory_link,
                               remove_directory_link, symlink_or_skip)


# Snapshot only the small, exclusively owned fixtures, with bounded traversal,
# retained file bytes and link text. This is not a concurrent-writer protocol.
SNAPSHOT_MAX_ENTRIES = 4096
SNAPSHOT_MAX_BYTES = 16 * 1024 * 1024
SNAPSHOT_MAX_LINK_CHARS = 32768


class ComparisonFixtureTest(unittest.TestCase):
    def test_identity_normalizes_configuration_newlines(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "data/common").mkdir(parents=True)
            (root / "worlds/atlas/presets").mkdir(parents=True)
            marker = root / comparison.MARKER
            identities = []
            for newline in (b"\n", b"\r\n", b"\r"):
                marker.write_bytes(b'{"controlled":true}' + newline)
                identities.append(comparison.identity(root))
            self.assertEqual(identities, [identities[0]] * 3)

    def test_identity_refuses_external_preset_references(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "data/common").mkdir(parents=True)
            presets = root / "worlds/atlas/presets"
            presets.mkdir(parents=True)
            (root / comparison.MARKER).write_text("controlled")
            for reference in ROOTED_REFERENCES + ESCAPING_REFERENCES:
                (presets / "default.json").write_text(json.dumps({
                    "generation_params": {"biomes": {"table": reference}}}))
                with self.subTest(reference=reference):
                    with self.assertRaisesRegex(ValueError, "fixture reference"):
                        comparison.identity(root)

    def test_preparation_refuses_unsafe_links_before_copy_or_source_changes(self):
        with tempfile.TemporaryDirectory() as temp:
            parent = Path(temp)
            base, head = parent / "base", parent / "head"
            common = base / "data/common"
            common.mkdir(parents=True)
            (base / "worlds/atlas/presets").mkdir(parents=True)
            outside = parent / "outside.json"
            outside.write_text("{}")
            symlink_or_skip(self, common / "systems.json", outside)
            with self.assertRaisesRegex(ValueError, "resolves outside root"):
                comparison.prepare(base, head, parent / "receipt.json")
            self.assertFalse((base / "perf-common").exists())
            self.assertFalse((head / "perf-common").exists())
            self.assertFalse((parent / "receipt.json").exists())

    def test_preparation_refuses_unsafe_preset_before_copy(self):
        with tempfile.TemporaryDirectory() as temp:
            base, head = Path(temp) / "base", Path(temp) / "head"
            (base / "data/common").mkdir(parents=True)
            presets = base / "worlds/atlas/presets"
            presets.mkdir(parents=True)
            for reference in ("../outside.json", r"a\b/../../..\biomes.json"):
                (presets / "default.json").write_text(json.dumps({
                    "generation_params": {"biomes": {"table": reference}}}))
                with self.subTest(reference=reference), self.assertRaisesRegex(ValueError, "escapes root"):
                    comparison.prepare(base, head, Path(temp) / "receipt.json")
                self.assertFalse((base / "perf-common").exists())
                self.assertFalse((head / "perf-common").exists())
                self.assertFalse((Path(temp) / "receipt.json").exists())

    def test_preparation_carries_validated_native_dependency_paths(self):
        reference = r"a\b/../..\biomes.json"
        native_name = "biomes.json" if os.name == "nt" else r"..\biomes.json"
        for present in (False, True):
            with self.subTest(present=present), tempfile.TemporaryDirectory() as temp:
                base, head = Path(temp) / "base", Path(temp) / "head"
                for checkout in (base, head):
                    source = checkout / "src/luminumbra_server/ServerWorldRunner.cpp"
                    source.parent.mkdir(parents=True)
                    source.write_text('    bool world_ready = false;\n'
                        '    world_system->debug_set_streaming_radius_cap(m_config.test_streaming_radius_cap);\n')
                common = base / "data/common"
                common.mkdir(parents=True)
                (common / "systems.json").write_text('{}')
                presets = base / "worlds/atlas/presets"
                presets.mkdir(parents=True)
                (presets / "default.json").write_text(json.dumps({
                    "generation_params": {"biomes": {"table": reference}}}))
                dependency = base / "data" / native_name
                if present:
                    dependency.write_bytes(b'{"biomes":[]}')
                schema = head / "src/luminumbra_common/core/ConfigSchema.json"
                schema.parent.mkdir(parents=True)
                schema.write_text('{"keys":[]}')
                receipt = Path(temp) / "receipt.json"
                comparison.prepare(base, head, receipt)
                self.assertTrue(receipt.is_file())
                for checkout in (base, head):
                    copied = checkout / "perf-common/data" / native_name
                    self.assertEqual(copied.exists(), present)
                    if present:
                        self.assertEqual(copied.read_bytes(), dependency.read_bytes())
                self.assertEqual(comparison.identity(base / "perf-common"),
                                 comparison.identity(head / "perf-common"))

    @staticmethod
    def comparison_checkouts(parent):
        base, head = parent / "base", parent / "head"
        for checkout in (base, head):
            source = checkout / "src/luminumbra_server/ServerWorldRunner.cpp"
            source.parent.mkdir(parents=True)
            source.write_text('    bool world_ready = false;\n'
                '    world_system->debug_set_streaming_radius_cap(m_config.test_streaming_radius_cap);\n')
        data = base / "data"
        (data / "common").mkdir(parents=True)
        (data / "common/systems.json").write_text('{}')
        (data / "common/biomes.json").write_text('{"biomes":[]}')
        (data / "extra").mkdir()
        document = lambda table: json.dumps({"generation_params": {"biomes": {"table": table}}})
        (data / "extra/branch.json").write_text(document("extra/leaf.json"))
        (data / "extra/leaf.json").write_text('{"biomes":[1]}')
        native_name = "biomes.json" if os.name == "nt" else r"..\biomes.json"
        (data / native_name).write_text('{"biomes":[2]}')
        presets = base / "worlds/atlas/presets"
        presets.mkdir(parents=True)
        for name, reference in (("default", "common/biomes.json"), ("extra", "extra/branch.json"),
                                ("native", r"a\b/../..\biomes.json"), ("missing", "extra/missing.json")):
            (presets / f"{name}.json").write_text(document(reference))
        schema = head / "src/luminumbra_common/core/ConfigSchema.json"
        schema.parent.mkdir(parents=True)
        schema.write_text('{"keys":[{"section":"sim","json_name":"active_regions"}]}')
        return base, head

    @staticmethod
    def tree_contents(root):
        # The caller may select a root alias, but descendant redirects must be
        # recorded without visiting their targets, including dangling junctions.
        result, pending, remaining = {}, [root], SNAPSHOT_MAX_BYTES
        while pending:
            for path in pending.pop().iterdir():
                if len(result) >= SNAPSHOT_MAX_ENTRIES:
                    raise ValueError("fixture snapshot entry limit exceeded")
                name = path.relative_to(root).as_posix()
                metadata = os.lstat(path)  # DirEntry.stat has zero Windows file IDs.
                attributes = metadata.st_file_attributes if os.name == "nt" else 0
                tag = metadata.st_reparse_tag if os.name == "nt" else 0
                reparse = attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT
                if stat.S_ISLNK(metadata.st_mode) or reparse:
                    # Win32 IO_REPARSE_TAG_SYMLINK and IO_REPARSE_TAG_MOUNT_POINT.
                    if reparse and tag not in (0xA000000C, 0xA0000003):
                        raise ValueError("unsupported fixture snapshot reparse tag")
                    target = os.readlink(path)
                    if len(target) > SNAPSHOT_MAX_LINK_CHARS:
                        raise ValueError("fixture snapshot link text limit exceeded")
                    identity = tuple(getattr(metadata, field) for field in (
                        "st_mode", "st_dev", "st_ino", "st_nlink", "st_size",
                        "st_mtime_ns", "st_ctime_ns"))
                    result[name] = ("link", (identity, attributes, tag, target))
                elif stat.S_ISDIR(metadata.st_mode):
                    result[name] = ("directory", None)
                    pending.append(path)
                elif stat.S_ISREG(metadata.st_mode):
                    if metadata.st_size > remaining:
                        raise ValueError("fixture snapshot byte limit exceeded")
                    with path.open("rb") as stream:
                        content = stream.read(remaining + 1)
                    if len(content) > remaining:
                        raise ValueError("fixture snapshot byte limit exceeded")
                    if len(content) != metadata.st_size:
                        raise ValueError("fixture snapshot file size changed")
                    remaining -= len(content)
                    result[name] = ("file", content)
                else:
                    raise ValueError("unsupported fixture snapshot entry type")
        return result

    def test_tree_contents_records_real_directory_links_and_cleanup(self):
        for target_kind in ("missing", "outside", "cycle"):
            with self.subTest(target_kind=target_kind), tempfile.TemporaryDirectory() as temp:
                parent = Path(temp).resolve()
                root = parent / "fixture"
                root.mkdir()
                (root / "empty").mkdir()
                (root / "ordinary.json").write_bytes(b"{}")
                outside = parent / "outside"
                outside.mkdir()
                sentinel = outside / "sentinel.json"
                sentinel.write_bytes(b"outside")
                target = {"missing": outside / "missing", "outside": outside,
                          "cycle": root}[target_kind]
                link = root / "redirect.json"
                directory_link(link, target)
                try:
                    before = self.tree_contents(root)
                    self.assertEqual(set(before), {"empty", "ordinary.json", "redirect.json"})
                    self.assertEqual(before["empty"], ("directory", None))
                    self.assertEqual(before["ordinary.json"], ("file", b"{}"))
                    self.assertEqual(before["redirect.json"][0], "link")
                    self.assertEqual(before["redirect.json"][1][-1], os.readlink(link))
                    self.assertEqual(self.tree_contents(root), before)
                    remove_directory_link(link)
                    directory_link(link, outside / "replacement")
                    self.assertNotEqual(self.tree_contents(root), before)
                finally:
                    remove_directory_link(link)
                with self.assertRaises(FileNotFoundError):
                    os.lstat(link)
                self.assertEqual(sentinel.read_bytes(), b"outside")

    def test_tree_contents_records_reparse_identity_without_target_io(self):
        # Metadata controls run everywhere; only directory_link tests exercise
        # native junctions on Windows. The stand-in directory must not be walked.
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            link = root / "redirect.json"
            link.mkdir()
            (link / "unvisited.json").write_bytes(b"outside")
            fields = dict(st_mode=stat.S_IFDIR, st_dev=1, st_ino=2, st_nlink=1,
                          st_size=0, st_mtime_ns=3, st_ctime_ns=4,
                          st_file_attributes=stat.FILE_ATTRIBUTE_REPARSE_POINT,
                          st_reparse_tag=0xA0000003)
            def capture(metadata, target="missing"):
                with mock.patch.object(os, "name", "nt"), \
                        mock.patch.object(os, "lstat", return_value=types.SimpleNamespace(**metadata)) as status, \
                        mock.patch.object(os, "readlink", return_value=target) as read, \
                        mock.patch.object(Path, "open", side_effect=AssertionError("target opened")):
                    result = self.tree_contents(root)
                status.assert_called_once_with(link)
                read.assert_called_once_with(link)
                self.assertEqual(set(result), {"redirect.json"})
                self.assertEqual(result["redirect.json"][0], "link")
                self.assertEqual(result["redirect.json"][1][-1], target)
                return result

            before = capture(fields)
            for field in fields:
                changed = {**fields, field: fields[field] + 1}
                if field == "st_mode":
                    changed[field] = stat.S_IFREG
                elif field == "st_file_attributes":
                    changed[field] |= stat.FILE_ATTRIBUTE_DIRECTORY
                elif field == "st_reparse_tag":
                    changed[field] = 0xA000000C
                with self.subTest(field=field):
                    self.assertNotEqual(capture(changed), before)
            self.assertNotEqual(capture(fields, "different"), before)

    def test_tree_contents_reparse_failures_are_not_omitted(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            link = root / "redirect.json"
            link.mkdir()
            metadata = dict(st_mode=stat.S_IFDIR, st_dev=1, st_ino=2, st_nlink=1,
                            st_size=0, st_mtime_ns=3, st_ctime_ns=4,
                            st_file_attributes=stat.FILE_ATTRIBUTE_REPARSE_POINT,
                            st_reparse_tag=0xA0000003)
            for failure in (PermissionError("denied"), OSError("partial reparse data")):
                with self.subTest(failure=failure), mock.patch.object(os, "name", "nt"), \
                        mock.patch.object(os, "lstat", return_value=types.SimpleNamespace(**metadata)), \
                        mock.patch.object(os, "readlink", side_effect=failure), \
                        self.assertRaises(type(failure)):
                    self.tree_contents(root)
            for tag in (0, 0x80000001):
                with self.subTest(tag=tag), mock.patch.object(os, "name", "nt"), \
                        mock.patch.object(os, "lstat", return_value=types.SimpleNamespace(
                            **{**metadata, "st_reparse_tag": tag})), \
                        mock.patch.object(os, "readlink", side_effect=AssertionError("unknown tag opened")), \
                        self.assertRaisesRegex(ValueError, "unsupported.*reparse tag"):
                    self.tree_contents(root)
            for field in ("st_ino", "st_file_attributes", "st_reparse_tag"):
                partial = {key: value for key, value in metadata.items() if key != field}
                with self.subTest(missing=field), mock.patch.object(os, "name", "nt"), \
                        mock.patch.object(os, "lstat", return_value=types.SimpleNamespace(**partial)), \
                        mock.patch.object(os, "readlink", return_value="missing"), \
                        self.assertRaises(AttributeError):
                    self.tree_contents(root)

    def test_tree_contents_uses_native_platform_metadata(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "empty").mkdir()
            # Windows-only fields are not reparse evidence on POSIX.
            metadata = types.SimpleNamespace(st_mode=stat.S_IFDIR,
                st_file_attributes=stat.FILE_ATTRIBUTE_REPARSE_POINT, st_reparse_tag=0)
            with mock.patch.object(os, "name", "posix"), \
                    mock.patch.object(os, "lstat", return_value=metadata), \
                    mock.patch.object(os, "readlink", side_effect=AssertionError("not a native reparse")):
                self.assertEqual(self.tree_contents(root), {"empty": ("directory", None)})
            self.assertEqual(self.tree_contents(root / "empty"), {})

    def test_tree_contents_metadata_and_file_read_errors_are_not_omitted(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            path = root / "ordinary.json"
            path.write_bytes(b"{}")
            for failure in (PermissionError("denied"), OSError("partial metadata"), FileNotFoundError("gone")):
                with self.subTest(failure=failure), \
                        mock.patch.object(os, "lstat", side_effect=failure), self.assertRaises(type(failure)):
                    self.tree_contents(root)
                with self.subTest(read_failure=failure), \
                        mock.patch.object(Path, "open", side_effect=failure), self.assertRaises(type(failure)):
                    self.tree_contents(root)
            for content in (b"{", b"{} "):
                with self.subTest(content=content), \
                        mock.patch.object(Path, "open", return_value=io.BytesIO(content)), \
                        self.assertRaisesRegex(ValueError, "file size changed"):
                    self.tree_contents(root)

    def test_tree_contents_refuses_nonregular_entries_without_open(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "entry").touch()
            for mode in (0, stat.S_IFIFO, stat.S_IFSOCK, stat.S_IFCHR, stat.S_IFBLK):
                with self.subTest(mode=mode), \
                        mock.patch.object(os, "lstat", return_value=types.SimpleNamespace(
                            st_mode=mode, st_file_attributes=0, st_reparse_tag=0)), \
                        mock.patch.object(Path, "open", side_effect=AssertionError("nonregular opened")), \
                        self.assertRaisesRegex(ValueError, "unsupported.*entry type"):
                    self.tree_contents(root)

    def test_tree_contents_enforces_entry_and_byte_budgets(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            first = root / "first.json"
            first.write_bytes(b"12")
            (root / "second.json").write_bytes(b"34")
            with mock.patch.dict(globals(), SNAPSHOT_MAX_ENTRIES=2, SNAPSHOT_MAX_BYTES=4):
                self.assertEqual(len(self.tree_contents(root)), 2)
            for limits, message in ((dict(SNAPSHOT_MAX_ENTRIES=1), "entry limit"),
                                    (dict(SNAPSHOT_MAX_BYTES=3), "byte limit")):
                with self.subTest(limits=limits), mock.patch.dict(globals(), limits), \
                        self.assertRaisesRegex(ValueError, message):
                    self.tree_contents(root)
            (root / "second.json").unlink()
            with mock.patch.dict(globals(), SNAPSHOT_MAX_BYTES=2), \
                    mock.patch.object(Path, "open", return_value=io.BytesIO(b"123")), \
                    self.assertRaisesRegex(ValueError, "byte limit"):
                self.tree_contents(root)

    def test_tree_contents_bounds_link_text(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            link = root / "redirect.json"
            directory_link(link, root / "missing")
            try:
                with mock.patch.dict(globals(), SNAPSHOT_MAX_LINK_CHARS=3), \
                        mock.patch.object(os, "readlink", return_value="abc"):
                    self.assertEqual(self.tree_contents(root)["redirect.json"][1][-1], "abc")
                with mock.patch.dict(globals(), SNAPSHOT_MAX_LINK_CHARS=3), \
                        mock.patch.object(os, "readlink", return_value="abcd"), \
                        self.assertRaisesRegex(ValueError, "link text limit"):
                    self.tree_contents(root)
            finally:
                remove_directory_link(link)

    def test_preparation_preserves_missing_optional_placement_identity(self):
        identities = []
        for dangling in (False, True):
            with self.subTest(dangling=dangling), tempfile.TemporaryDirectory() as temp:
                parent = Path(temp).resolve()
                base, head = self.comparison_checkouts(parent)
                common = base / "data/common"
                pool = common / "structures/cairn"
                pool.mkdir(parents=True)
                (pool / "cairn.json").write_text('{"weight":1}')
                placement = pool / ("PLACEMENT.JSON" if os.name == "nt" else "placement.json")
                expected = comparison.fixture_hash(common, scope="headless-server")
                if dangling:
                    directory_link(placement, pool / "missing.json")
                try:
                    self.assertEqual(comparison.fixture_hash(common, scope="headless-server"), expected)
                    receipt = parent / "receipt.json"
                    comparison.prepare(base, head, receipt)
                    records = json.loads(receipt.read_text())["runs"]
                    prepared = []
                    for checkout, record in zip((base, head), records):
                        root = checkout / "perf-common"
                        self.assertFalse((root / "data/common/structures/cairn/placement.json").exists())
                        self.assertFalse((root / "data/common/structures/cairn/placement.json").is_symlink())
                        with self.assertRaises(FileNotFoundError):
                            os.lstat(root / "data/common/structures/cairn/placement.json")
                        self.assertEqual((root / "data/common/structures/cairn/cairn.json").read_bytes(),
                                         (pool / "cairn.json").read_bytes())
                        prepared.append(comparison.identity(root))
                        self.assertEqual(record["fixture_hash"], prepared[-1])
                    self.assertEqual(prepared[0], prepared[1])
                    identities.append(prepared[0])
                finally:
                    if dangling:
                        remove_directory_link(placement)
        self.assertEqual(identities[0], identities[1])

    def test_preparation_copies_authored_optional_placement(self):
        with tempfile.TemporaryDirectory() as temp:
            parent = Path(temp).resolve()
            base, head = self.comparison_checkouts(parent)
            pool = base / "data/common/structures/cairn"
            pool.mkdir(parents=True)
            (pool / "cairn.json").write_text('{"weight":1}')
            placement = pool / "placement.json"
            placement.write_text('{"spacing":96,"salt":101}')
            comparison.prepare(base, head, parent / "receipt.json")
            for checkout in (base, head):
                root = checkout / "perf-common"
                copied = root / "data/common/structures/cairn/placement.json"
                self.assertEqual(copied.read_bytes(), placement.read_bytes())
                before = comparison.identity(root)
                copied.write_text('{"spacing":97,"salt":101}')
                self.assertNotEqual(comparison.identity(root), before)

    def test_preparation_refuses_missing_required_piece_and_outside_placement_before_copy(self):
        for name, outside in (("cairn.json", False), ("placement.json", True)):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temp:
                parent = Path(temp).resolve()
                base, head = self.comparison_checkouts(parent)
                pool = base / "data/common/structures/cairn"
                pool.mkdir(parents=True)
                target = parent / "outside-missing.json" if outside else pool / "missing.json"
                link = pool / name
                directory_link(link, target)
                try:
                    before = [self.tree_contents(checkout) for checkout in (base, head)]
                    receipt = parent / "receipt.json"
                    with self.assertRaises((ValueError, FileNotFoundError)):
                        comparison.prepare(base, head, receipt)
                    self.assertEqual([self.tree_contents(checkout) for checkout in (base, head)], before)
                    self.assertFalse(receipt.exists())
                finally:
                    remove_directory_link(link)

    def test_preparation_uses_one_data_root_for_links_and_caller_aliases(self):
        for linked_data in (False, True):
            for caller_alias in (False, True):
                with self.subTest(linked_data=linked_data, caller_alias=caller_alias), \
                        tempfile.TemporaryDirectory() as temp:
                    parent = Path(temp).resolve()
                    base, head = self.comparison_checkouts(parent)
                    data_before = self.tree_contents(base / "data")
                    presets_before = self.tree_contents(base / "worlds/atlas/presets")
                    sources = {checkout: (checkout / "src/luminumbra_server/ServerWorldRunner.cpp").read_bytes()
                               for checkout in (base, head)}
                    links = []
                    try:
                        if linked_data:
                            (base / "data").rename(base / "assets")
                            directory_link(base / "data", base / "assets")
                            links.append(base / "data")
                        selected = []
                        for checkout in (base, head):
                            alias = parent / (checkout.name + "-selected")
                            if caller_alias:
                                directory_link(alias, checkout)
                                links.append(alias)
                            selected.append(alias if caller_alias else checkout)
                        receipt = parent / "receipt.json"
                        comparison.prepare(*selected, receipt)
                        record = json.loads(receipt.read_text())
                        self.assertEqual(record["schema"], "luminumbra.server_comparison_fixture.v1")
                        self.assertEqual(len(record["runs"]), 2)
                        identities = []
                        for checkout, requested, run in zip((base, head), selected, record["runs"]):
                            root = requested / "perf-common"
                            identities.append(comparison.identity(root))
                            self.assertEqual(run["checkout"], str(requested))
                            self.assertEqual(run["fixture_hash"], identities[-1])
                            self.assertEqual(run["source"], "src/luminumbra_server/ServerWorldRunner.cpp".replace("/", os.sep))
                            source = checkout / run["source"]
                            self.assertEqual(run["source_before_sha256"], hashlib.sha256(sources[checkout]).hexdigest())
                            self.assertEqual(run["source_after_sha256"], hashlib.sha256(source.read_bytes()).hexdigest())
                            self.assertNotEqual(run["source_before_sha256"], run["source_after_sha256"])
                            for gate in comparison.GATES:
                                self.assertIn(f"Set{gate}Enabled(false)", source.read_text())
                            self.assertIn("SetWaterHydrology(false, 0, 0)", source.read_text())
                            self.assertIn(comparison.MARKER, source.read_text())
                            for name, (kind, content) in data_before.items():
                                copied = root / "data" / name
                                if kind == "file" and name != "common/systems.json":
                                    self.assertEqual(copied.read_bytes(), content)
                            self.assertFalse((root / "data/extra/missing.json").exists())
                            self.assertEqual(self.tree_contents(root / "worlds/atlas/presets"), presets_before)
                            systems = json.loads((root / "data/common/systems.json").read_text())
                            self.assertFalse(systems["sim"]["active_regions"]["enabled"])
                            configuration = json.loads((root / comparison.MARKER).read_text())
                            self.assertEqual(configuration["schema"], "luminumbra.controlled_server.v1")
                            self.assertFalse(configuration["gates"])
                            self.assertFalse(configuration["finite_hydrology"])
                            self.assertEqual(configuration["autosave_ticks"], 0)
                            # Identity must also accept the selected data directory as a link.
                            (root / "data").rename(root / "assets")
                            directory_link(root / "data", root / "assets")
                            links.append(root / "data")
                            self.assertEqual(comparison.identity(root), identities[-1])
                            leaf = root / "data/extra/leaf.json"
                            leaf.write_text('{"biomes":[3]}')
                            self.assertNotEqual(comparison.identity(root), identities[-1])
                        self.assertEqual(identities[0], identities[1])
                        self.assertEqual(self.tree_contents(base / "data"), data_before)
                        self.assertEqual(self.tree_contents(base / "worlds/atlas/presets"), presets_before)
                    finally:
                        for link in reversed(links):
                            remove_directory_link(link)

    def test_data_root_alias_refuses_dependencies_outside_data_before_publication(self):
        with tempfile.TemporaryDirectory() as temp:
            parent = Path(temp).resolve()
            base, head = self.comparison_checkouts(parent)
            (base / "data").rename(base / "assets")
            directory_link(base / "data", base / "assets")
            try:
                outside = base / "outside-data"
                outside.mkdir()
                (outside / "biomes.json").write_text('{"biomes":[]}')
                directory_link(base / "assets/escape", outside)
                try:
                    preset = base / "worlds/atlas/presets/default.json"
                    preset.write_text(json.dumps({"generation_params": {
                        "biomes": {"table": "escape/biomes.json"}}}))
                    before = [self.tree_contents(checkout) for checkout in (base, head)]
                    receipt = parent / "receipt.json"
                    with self.assertRaisesRegex(ValueError, "resolves outside root"):
                        comparison.prepare(base, head, receipt)
                    self.assertEqual([self.tree_contents(checkout) for checkout in (base, head)], before)
                    self.assertFalse((base / "perf-common").exists())
                    self.assertFalse((head / "perf-common").exists())
                    self.assertFalse(receipt.exists())
                finally:
                    remove_directory_link(base / "assets/escape")
            finally:
                remove_directory_link(base / "data")

    def test_inputs_match_and_saves_do_not_change_identity(self):
        with tempfile.TemporaryDirectory() as temp:
            base, head = Path(temp) / "base", Path(temp) / "head"
            for checkout in (base, head):
                source = checkout / "src/luminumbra_server/ServerWorldRunner.cpp"
                source.parent.mkdir(parents=True)
                source.write_text('    bool world_ready = false;\n'
                    '    world_system->debug_set_streaming_radius_cap(m_config.test_streaming_radius_cap);\n')
            common = base / "data/common"
            common.mkdir(parents=True)
            (common / "systems.json").write_text('{"sim":{"erosion":{"enabled":true}}}')
            presets = base / "worlds/atlas/presets"
            presets.mkdir(parents=True)
            (presets / "default.json").write_text(json.dumps({"schema_rev": 6,
                "generation_params": {"biomes": {"table": "extra/biomes.json"}}}))
            extra = base / "data/extra"
            extra.mkdir()
            (extra / "biomes.json").write_text('{"biomes":[]}')
            schema = head / "src/luminumbra_common/core/ConfigSchema.json"
            schema.parent.mkdir(parents=True)
            schema.write_text(json.dumps({"keys": [dict(section="sim", json_name="erosion"),
                dict(section="sim", json_name="active_regions"), dict(section="render", json_name="taau")]}), encoding="utf-8-sig")
            session_header = head / "src/luminumbra_common/world/GameSession.h"
            session_header.parent.mkdir(parents=True)
            session_header.write_text("void SetFiniteHydrologyEnabled(bool enabled);")
            original = (common / "systems.json").read_bytes()
            receipt = Path(temp) / "receipt.json"
            comparison.prepare(base, head, receipt)
            self.assertEqual(comparison.identity(base / "perf-common"),
                             comparison.identity(head / "perf-common"))
            for checkout in (base, head):
                root = checkout / "perf-common"
                flags = json.loads((root / "data/common/systems.json").read_text())
                self.assertFalse(flags["sim"]["active_regions"]["enabled"])
                self.assertFalse(flags["render"]["taau"]["enabled"])
                self.assertEqual((root / "data/extra/biomes.json").read_bytes(),
                                 (extra / "biomes.json").read_bytes())
                before = comparison.identity(root)
                saves = root / "worlds/saves"
                saves.mkdir()
                (saves / "generated.json").write_text('{"tick":30}')
                self.assertEqual(before, comparison.identity(root))
                (root / "data/extra/biomes.json").write_text('{"biomes":[1]}')
                self.assertNotEqual(before, comparison.identity(root))
                before = comparison.identity(root)
                (root / "worlds/atlas/presets/default.json").write_text('{"schema_rev":7}')
                self.assertNotEqual(before, comparison.identity(root))
                source = (checkout / "src/luminumbra_server/ServerWorldRunner.cpp").read_text()
                self.assertIn('SetAetherStateEnabled(false)', source)
                self.assertIn('SetWaterHydrology(false, 0, 0)', source)
                self.assertIn(comparison.MARKER, source)
                self.assertEqual('SetFiniteHydrologyEnabled(false)' in source, checkout == head)
            self.assertEqual(original, (common / "systems.json").read_bytes())
            runs = json.loads(receipt.read_text())["runs"]
            self.assertNotEqual(runs[0]["source_before_sha256"], runs[0]["source_after_sha256"])
            with self.assertRaises(ValueError):
                comparison.prepare(base, head, receipt)


if __name__ == "__main__":
    unittest.main()
