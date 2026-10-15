#!/usr/bin/env python3
"""Structural regression source. Payloads here intentionally have no native meaning."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
import stat
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

import refusal_fixtures as fixtures
import save_receipt


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def encode(value):
    return (json.dumps(value, sort_keys=True, indent=2) + "\n").encode("utf-8")


def region(version=2, records=None):
    records = records or [(5, 0, 1, 8, b"opaque"), (6, 2, 0, 4, b"data")]
    return (b"LMR1" + struct.pack("<HH", version, len(records)) +
            b"".join(struct.pack("<QBBII", identity, lod, flags, raw, len(payload))
                     for identity, lod, flags, raw, payload in records) +
            b"".join(payload for _, _, _, _, payload in records))


def run(arguments):
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        return fixtures.main(arguments)


class RefusalFixtureContract(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.engine = self.root / "engine"
        self.output = self.root / "output"
        self.save = self.source / "worlds/saves/example"
        self.metadata = (b'{\n  "name" : "Example", "container_version" : 2,\n'
                         b'  "seed": "123", "worldType": "flat", "creationTime": 0\n}\n')
        write(self.save / "world_info.json", self.metadata)
        write(self.save / "preset.json", b'{"terrain": "structural-example"}\n')
        write(self.save / fixtures.WORLD_MANIFEST, encode({
            "schema": "luminumbra.persistence.world_manifest.v1",
            "container": "LMR1", "container_version": 2,
        }))
        # Lexical selection differs from numeric coordinate ordering.
        self.first = "chunks/region/r.-1.0.lmr"
        write(self.save / self.first, region())
        write(self.save / "chunks/region/r.2.0.lmr", region(records=[(9, 1, 0, 2, b"tail")]))
        self.header = self.engine / fixtures.HEADER
        write(self.header, b"static constexpr std::uint16_t kContainerVersion = 2;\n")
        self.revision = mock.patch.object(fixtures, "source_revision", return_value="a" * 40)
        self.revision.start()
        self.addCleanup(self.revision.stop)

    def create(self, output=None):
        return fixtures.create(self.source, output or self.output, self.engine)

    def rejected(self):
        self.assertEqual(run(["create", "--source-root", str(self.source),
                              "--out", str(self.output), "--engine-root", str(self.engine)]), 2)
        self.assertFalse(self.output.exists())

    def load_manifest(self):
        return json.loads((self.output / fixtures.MANIFEST).read_bytes())

    def store_manifest(self, manifest):
        (self.output / fixtures.MANIFEST).write_bytes(encode(manifest))

    def rebind_variant(self, manifest, identity):
        """Forge valid inventories so semantic checking must catch wrong bytes."""
        entry = next(item for item in manifest["variants"] if item["id"] == identity)
        entry["inventory"] = save_receipt.snapshot(self.output / "refusals" / identity)
        path = entry["mutation"]["path"]
        entry["mutation"]["after"] = fixtures._file_identity(entry["inventory"], path)
        self.store_manifest(manifest)

    def assert_verify_refuses(self):
        self.assertEqual(run(["verify", str(self.output)]), 2)

    def test_deterministic_two_outputs_and_exact_six_mutations(self):
        source_before = save_receipt.snapshot(self.source)
        first = self.create()
        other = self.root / "other"
        second = self.create(other)
        self.assertEqual(first, second)
        self.assertEqual((self.output / fixtures.MANIFEST).read_bytes(),
                         (other / fixtures.MANIFEST).read_bytes())
        self.assertEqual(save_receipt.snapshot(self.output), save_receipt.snapshot(other))
        self.assertEqual(fixtures.verify(self.output), first)
        self.assertEqual(run(["verify", str(other)]), 0)
        baseline = self.output / "worlds/saves/example"
        self.assertEqual(first["source"], first["baseline"])
        self.assertEqual(first["source"], save_receipt.snapshot(self.save))
        self.assertEqual((baseline / "world_info.json").read_bytes(), self.metadata)
        self.assertEqual(source_before, save_receipt.snapshot(self.source))
        self.assertEqual(first["source_revision"], "a" * 40)
        self.assertEqual([entry["id"] for entry in first["variants"]], list(fixtures.VARIANTS))
        for entry in first["variants"]:
            identity = entry["id"]
            clone = self.output / "refusals" / identity
            path = entry["mutation"]["path"]
            differences = save_receipt.compare(first["source"], entry["inventory"])
            self.assertEqual(sum(len(paths) for paths in differences.values()), 1)
            self.assertEqual((clone / fixtures.WORLD_MANIFEST).read_bytes(),
                             (self.save / fixtures.WORLD_MANIFEST).read_bytes())
            self.assertEqual(entry["world_id"], identity)
            self.assertEqual(entry["expected_exit"], 2)
            self.assertEqual(entry["world_type"], "flat")
            self.assertEqual(entry["runtime_requirements"]["atlas_preset_absent"],
                             "flat" if identity == "missing-preset" else None)
            if identity in ("obsolete-metadata", "future-metadata"):
                value = 1 if identity == "obsolete-metadata" else 3
                self.assertEqual((clone / path).read_bytes(),
                                 self.metadata.replace(b'"container_version" : 2',
                                                       f'"container_version" : {value}'.encode()))
                self.assertEqual(entry["reason"], {"kind": identity, "version": value})
            elif identity == "corrupt-metadata":
                self.assertEqual((clone / path).read_bytes(), b"{")
            elif identity.startswith("missing-"):
                self.assertFalse((clone / path).exists())
                self.assertIsNone(entry["mutation"]["after"])
            else:
                self.assertEqual(path, self.first)
                before = (self.save / path).read_bytes()
                after = (clone / path).read_bytes()
                self.assertEqual(after, before[:-1])
                self.assertEqual(after[:8 + 2 * 18], before[:8 + 2 * 18])
        manifest_bytes = (self.output / fixtures.MANIFEST).read_text(encoding="utf-8")
        self.assertNotIn(str(self.root), manifest_bytes)

    def test_source_version_disagreement(self):
        for relative, data in (
            ("world_info.json", self.metadata.replace(b'version" : 2', b'version" : 3')),
            (fixtures.WORLD_MANIFEST, encode({"schema": "luminumbra.persistence.world_manifest.v1",
                                            "container": "LMR1", "container_version": 3})),
            (self.first, region(version=3)),
        ):
            path = self.save / relative
            before = path.read_bytes()
            with self.subTest(relative=relative):
                path.write_bytes(data)
                self.rejected()
                path.write_bytes(before)

    def test_invalid_region_framing_headers_and_lengths(self):
        path = self.save / self.first
        good = path.read_bytes()
        malformed = [b"", good[:7], b"WRNG" + good[4:], good[:20], good[:-1], good + b"x",
                     b"LMR1" + struct.pack("<HH", 2, 0),
                     b"LMR1" + struct.pack("<HH", 2, 3) + good[8:],
                     region(records=[(1, 3, 0, 2, b"xx")]),
                     region(records=[(1, 0, 4, 2, b"xx")]),
                     region(records=[(1, 0, 0, 0, b"xx")]),
                     region(records=[(1, 0, 0, 2, b"")]),
                     region(records=[(1, 0, 0, 2, b"xx"), (1, 0, 0, 2, b"yy")])]
        for index, data in enumerate(malformed):
            with self.subTest(index=index):
                path.write_bytes(data)
                self.rejected()
        path.write_bytes(good)

    def test_unknown_artifacts_and_noncanonical_regions(self):
        for relative in ("extra.txt", "chunks/region/r.01.0.lmr", "chunks/region/r.-0.0.lmr",
                         "chunks/region/r.2147483648.0.lmr", "chunks/region/nested/r.0.0.lmr"):
            with self.subTest(relative=relative):
                path = self.save / relative
                write(path, region())
                self.rejected()
                path.unlink()
                if path.parent.name == "nested":
                    path.parent.rmdir()
        empty = self.source / "empty"
        empty.mkdir()
        self.rejected()
        empty.rmdir()
        other = self.source / "worlds/saves/second"
        other.mkdir()
        self.rejected()

    def test_invalid_json_duplicate_keys_nonfinite_and_id_fields(self):
        path = self.save / "world_info.json"
        for data in (b"{", b'[]', self.metadata.replace(b'"seed": "123"', b'"seed": NaN'),
                     self.metadata.replace(b'"seed": "123"', b'"seed": 1e999'),
                     self.metadata.replace(b'"seed": "123"', b'"seed": 1, "seed": 2'),
                     self.metadata.replace(b'"seed": "123"', b'"id": "x"'),
                     self.metadata.replace(b'"seed": "123"', b'"worldId": "x"'),
                     self.metadata.replace(b'"seed": "123"', b'"nested": {"id": "x"}'),
                     self.metadata.replace(b'version" : 2', b'version" : true')):
            with self.subTest(data=data):
                path.write_bytes(data)
                self.rejected()
        path.write_bytes(self.metadata)
        for relative in ("preset.json", fixtures.WORLD_MANIFEST):
            target = self.save / relative
            original = target.read_bytes()
            target.write_bytes(b'{"x": 0, "x": 1}')
            self.rejected()
            target.write_bytes(original)

    def test_missing_and_ambiguous_engine_version(self):
        for data in (b"", b"// static constexpr std::uint16_t kContainerVersion = 2;\n",
                     b"constexpr std::uint16_t kContainerVersion = 2;\n" * 2,
                     b"constexpr std::uint16_t kContainerVersion = get_version();\n"):
            with self.subTest(data=data):
                self.header.write_bytes(data)
                self.rejected()
        self.header.unlink()
        self.rejected()

    def test_engine_comments_and_references_are_not_declarations(self):
        self.header.write_bytes(
            b'// constexpr std::uint16_t kContainerVersion = 9;\n'
            b'/* constexpr std::uint16_t kContainerVersion = 8; */\n'
            b'static constexpr std::uint16_t kContainerVersion = 2;\n'
            b'int version() { return kContainerVersion; }\n')
        self.assertEqual(fixtures.engine_version(self.engine), 2)
        with self.header.open("ab") as handle:
            handle.write(b"constexpr unsigned kContainerVersion = 2;\n")
        self.rejected()

    def test_version_token_selection_preserves_nested_and_escaped_json(self):
        metadata = (b'{"nested":{"container_version":77}, "name":"\\u00e9", '
                    b'"worldType":"flat", "\\u0063ontainer_version" : 2}\n')
        (self.save / "world_info.json").write_bytes(metadata)
        self.create()
        for identity, version in (("obsolete-metadata", 1), ("future-metadata", 3)):
            target = self.output / "refusals" / identity / "world_info.json"
            self.assertEqual(target.read_bytes(), metadata.replace(b'" : 2}',
                                                                    f'" : {version}}}'.encode()))
        self.assertEqual(run(["verify", str(self.output)]), 0)

    def test_source_missing_required_files_or_all_regions(self):
        for relative in ("world_info.json", "preset.json", fixtures.WORLD_MANIFEST, self.first):
            target = self.save / relative
            data = target.read_bytes()
            target.unlink()
            if relative == self.first:
                other = self.save / "chunks/region/r.2.0.lmr"
                other_data = other.read_bytes()
                other.unlink()
                self.rejected()
                other.write_bytes(other_data)
            else:
                self.rejected()
            target.write_bytes(data)

    def test_existing_outputs_and_overlap_are_never_removed(self):
        for populated in (False, True):
            self.output.mkdir()
            if populated:
                write(self.output / "keep", b"preserve")
            with self.assertRaises(fixtures.FixtureError):
                self.create()
            if populated:
                self.assertEqual((self.output / "keep").read_bytes(), b"preserve")
                (self.output / "keep").unlink()
            self.output.rmdir()
        for output in (self.source, self.source / "new", self.save / "new", self.root):
            with self.subTest(output=output), self.assertRaises(fixtures.FixtureError):
                self.create(output)
        # A new output ancestor of the source is impossible without an existing
        # output; equality and both containment directions are checked above.
        self.assertEqual((self.save / "world_info.json").read_bytes(), self.metadata)

    def test_output_inside_engine_checkout_is_allowed(self):
        output = self.engine / "fixtures"
        self.create(output)
        self.assertEqual(fixtures.verify(output)["container_version"], 2)

    def test_generated_world_identifier_with_leading_hyphen(self):
        replacement = self.save.with_name("-example")
        self.save.rename(replacement)
        manifest = self.create()
        self.assertEqual(manifest["source_world_id"], "-example")
        self.assertEqual(fixtures.verify(self.output), manifest)

    def test_symlinks_in_tree_root_and_raw_ancestors(self):
        link = self.root / "link"
        try:
            link.symlink_to(self.source, target_is_directory=True)
        except (OSError, NotImplementedError):
            self.skipTest("symlink creation unavailable")
        for source in (link, link / ".." / "source"):
            with self.subTest(source=source), self.assertRaises(fixtures.FixtureError):
                fixtures.create(source, self.output, self.engine)
        with self.assertRaises(fixtures.FixtureError):
            self.create(link / ".." / "new")
        (self.save / "link").symlink_to(self.save / "preset.json")
        self.rejected()
        (self.save / "link").unlink()
        dangling = self.output
        dangling.symlink_to(self.root / "absent")
        with self.assertRaises(fixtures.FixtureError):
            self.create()
        self.assertTrue(dangling.is_symlink())

    def test_special_file_and_reparse_attribute_are_refused(self):
        with self.assertRaises(fixtures.FixtureError):
            fixtures._regular_status(SimpleNamespace(st_mode=stat.S_IFDIR, st_file_attributes=0x400))
        if not hasattr(os, "mkfifo"):
            self.skipTest("FIFO creation unavailable; reparse attribute checked")
        special = self.save / "special"
        try:
            os.mkfifo(special)
        except OSError:
            self.skipTest("FIFO creation unavailable; reparse attribute checked")
        self.rejected()

    def test_cleanup_injected_copy_and_manifest_failures_only_created_root(self):
        sentinel = self.root / "keep"
        sentinel.write_bytes(b"outside")
        original = save_receipt.snapshot(self.source)
        real_copy = fixtures._copy_file
        calls = 0

        def fail_after_copy(*arguments):
            nonlocal calls
            real_copy(*arguments)
            calls += 1
            if calls == 2:
                raise OSError("injected copy failure")

        with mock.patch.object(fixtures, "_copy_file", side_effect=fail_after_copy):
            self.rejected()
        with mock.patch.object(fixtures.json, "dump", side_effect=OSError("injected publish failure")):
            self.rejected()
        self.assertEqual(sentinel.read_bytes(), b"outside")
        self.assertEqual(save_receipt.snapshot(self.source), original)

    def test_manifest_tampering_strict_schema_types_and_inventory(self):
        self.create()
        original = self.load_manifest()
        mutations = [lambda m: m.update(schema="wrong"), lambda m: m.update(extra=True),
                     lambda m: m.update(source_revision="short"),
                     lambda m: m.update(source_revision=123),
                     lambda m: m.update(container_version=True),
                     lambda m: m["variants"][0].update(expected_exit=1),
                     lambda m: m["variants"][0].update(expected_exit=2.0),
                     lambda m: m["variants"][0].update(id="unknown-variant"),
                     lambda m: m["variants"].reverse(),
                     lambda m: m["variants"][0]["reason"].update(version=99),
                     lambda m: m["source"].update(tree_digest="0" * 64),
                     lambda m: m["variants"][1]["inventory"].update(file_count=0),
                     lambda m: m["variants"][4]["runtime_requirements"].update(atlas_preset_absent=None),
                     lambda m: m["variants"][0]["mutation"].update(path="preset.json")]
        for index, mutate in enumerate(mutations):
            with self.subTest(index=index):
                manifest = json.loads(json.dumps(original))
                mutate(manifest)
                self.store_manifest(manifest)
                self.assert_verify_refuses()
        self.store_manifest(original)
        text = (self.output / fixtures.MANIFEST).read_text(encoding="utf-8")
        (self.output / fixtures.MANIFEST).write_text(
            text.replace('"container_version": 2', '"container_version": 2, "container_version": 2', 1),
            encoding="utf-8")
        self.assert_verify_refuses()

    def test_wrong_mutation_bytes_even_with_rebound_inventory(self):
        self.create()
        original_manifest = self.load_manifest()
        for identity, bad in (("obsolete-metadata", self.metadata.replace(b'version" : 2', b'version" : 0')),
                              ("future-metadata", self.metadata.replace(b'version" : 2', b'version" : 4')),
                              ("corrupt-metadata", b"}")):
            with self.subTest(identity=identity):
                target = self.output / "refusals" / identity / "world_info.json"
                good = target.read_bytes()
                target.write_bytes(bad)
                self.rebind_variant(json.loads(json.dumps(original_manifest)), identity)
                self.assert_verify_refuses()
                target.write_bytes(good)
                self.store_manifest(original_manifest)
        target = self.output / "refusals/truncated-region" / self.first
        good = target.read_bytes()
        for bad in (good[:7], good[:-1], b"X" + good[1:], good[:-1] + b"X"):
            with self.subTest(bad=bad):
                target.write_bytes(bad)
                self.rebind_variant(json.loads(json.dumps(original_manifest)), "truncated-region")
                self.assert_verify_refuses()
        target.write_bytes(good)
        self.store_manifest(original_manifest)
        # Changing the second region instead of the lexical first also refuses.
        target.write_bytes((self.save / self.first).read_bytes())
        other = self.output / "refusals/truncated-region/chunks/region/r.2.0.lmr"
        other.write_bytes(other.read_bytes()[:-1])
        self.rebind_variant(original_manifest, "truncated-region")
        self.assert_verify_refuses()

    def test_wrong_missing_mutation_baseline_change_and_unknown_output(self):
        self.create()
        manifest = self.load_manifest()
        clone = self.output / "refusals/missing-preset"
        write(clone / "preset.json", (self.save / "preset.json").read_bytes())
        (clone / "world_info.json").unlink()
        self.rebind_variant(manifest, "missing-preset")
        self.assert_verify_refuses()
        write(clone / "world_info.json", self.metadata)
        (clone / "preset.json").unlink()
        self.store_manifest(fixtures._expected_manifest(self.output, "example", 2, "a" * 40))
        baseline = self.output / "worlds/saves/example/preset.json"
        before = baseline.read_bytes()
        baseline.write_bytes(b"{}")
        self.assert_verify_refuses()
        baseline.write_bytes(before)
        write(self.output / "extra", b"unknown")
        self.assert_verify_refuses()
        (self.output / "extra").unlink()
        (self.output / "empty").mkdir()
        self.assert_verify_refuses()

    def test_source_revision_production_command_is_read_only_full_head(self):
        self.revision.stop()
        result = SimpleNamespace(stdout=("b" * 40 + "\n").encode())
        with mock.patch.object(fixtures.subprocess, "run", return_value=result) as command:
            self.assertEqual(fixtures.source_revision(self.engine), "b" * 40)
            self.assertEqual(command.call_args.args[0],
                             ["git", "-C", str(self.engine), "rev-parse", "--verify", "HEAD^{commit}"])
            result.stdout = b"abbreviated\n"
            with self.assertRaises(fixtures.FixtureError):
                fixtures.source_revision(self.engine)


class PublicSourceContract(unittest.TestCase):
    def test_owned_sources_pass_public_guard_and_guard_detects_sensitive_literals(self):
        root = Path(__file__).resolve().parents[2]
        spec = importlib.util.spec_from_file_location(
            "refusal_fixture_public_guard", root / "tools/ci/validate_public_tree.py")
        guard = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = guard
        self.addCleanup(sys.modules.pop, spec.name, None)
        spec.loader.exec_module(guard)
        for relative in ("tools/persistence/refusal_fixtures.py",
                         "tools/persistence/test_refusal_fixtures.py", "docs/refusal-fixtures.md"):
            self.assertEqual(guard.inspect_path(relative), [])
            self.assertEqual(guard.inspect_text(relative, (root / relative).read_text(encoding="utf-8")), [])
        # Assemble negative examples so this public regression source is clean.
        sensitive = ["/" + "home" + "/" + "example" + "/" + "work",
                     "C:" + "\\" + "Users" + "\\" + "example",
                     "gh" + "p_" + "a" * 32, "s" + "k-" + "a" * 25,
                     "AK" + "IA" + "A" * 16,
                     "-----" + "BEGIN " + "PRIVATE KEY" + "-----"]
        for text in sensitive:
            self.assertTrue(guard.inspect_text("example.txt", text))


if __name__ == "__main__":
    unittest.main()
