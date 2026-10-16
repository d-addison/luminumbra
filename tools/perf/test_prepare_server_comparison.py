#!/usr/bin/env python3
"""Controlled-workload identity and isolation contracts; stdlib only."""
import json
import os
from pathlib import Path
import tempfile
import unittest

import prepare_server_comparison as comparison
from test_fixture_hash import ROOTED_REFERENCES, ESCAPING_REFERENCES, symlink_or_skip


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
