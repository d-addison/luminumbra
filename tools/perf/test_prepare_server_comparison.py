#!/usr/bin/env python3
"""Controlled-workload identity and isolation contracts; stdlib only."""
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest

import prepare_server_comparison as comparison
from test_fixture_hash import (ROOTED_REFERENCES, ESCAPING_REFERENCES, directory_link,
                               remove_directory_link, symlink_or_skip)


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
        return {path.relative_to(root).as_posix():
                ("link", os.readlink(path)) if path.is_symlink() else
                ("directory", None) if path.is_dir() else ("file", path.read_bytes())
                for path in root.rglob("*")}

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
