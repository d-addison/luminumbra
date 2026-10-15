#!/usr/bin/env python3
"""Controlled-workload identity and isolation contracts; stdlib only."""
import json
from pathlib import Path
import tempfile
import unittest

import prepare_server_comparison as comparison


class ComparisonFixtureTest(unittest.TestCase):
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
            (presets / "default.json").write_text('{"schema_rev":6}')
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
                before = comparison.identity(root)
                saves = root / "worlds/saves"
                saves.mkdir()
                (saves / "generated.json").write_text('{"tick":30}')
                self.assertEqual(before, comparison.identity(root))
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
