#!/usr/bin/env python3
"""Controlled-workload identity and isolation contracts; stdlib only."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import capture_sim_budget as capture
import prepare_server_comparison as comparison


class ComparisonFixtureTest(unittest.TestCase):
    @staticmethod
    def write_preparation_fixture(root):
        base, head = root / "base", root / "head"
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
        for preset in capture.PRESETS:
            (presets / f"{preset}.json").write_text('{"schema_rev":6}')
        schema = head / "src/luminumbra_common/core/ConfigSchema.json"
        schema.parent.mkdir(parents=True)
        schema.write_text(json.dumps({"keys": [dict(section="sim", json_name="erosion"),
            dict(section="sim", json_name="active_regions"), dict(section="render", json_name="taau")]}), encoding="utf-8-sig")
        session_header = head / "src/luminumbra_common/world/GameSession.h"
        session_header.parent.mkdir(parents=True)
        session_header.write_text("void SetFiniteHydrologyEnabled(bool enabled);")
        return base, head, root / "receipt.json"

    @staticmethod
    def snapshot(root):
        return {path.relative_to(root).as_posix(): path.read_bytes() if path.is_file() else None
                for path in root.rglob("*")}

    @staticmethod
    def write_identity_fixture(root):
        common = root / "data/common"
        common.mkdir(parents=True)
        (common / "systems.json").write_text('{"sim":{}}', encoding="utf-8")
        (root / comparison.MARKER).write_text('{}\n', encoding="utf-8")
        presets = root / "worlds/atlas/presets"
        presets.mkdir(parents=True)
        for preset in capture.PRESETS:
            (presets / f"{preset}.json").write_text(json.dumps({
                "generation_params": {"biomes": {"table": "extra/biomes.json"}}}),
                encoding="utf-8")
        table = root / "data/extra/biomes.json"
        table.parent.mkdir()
        table.write_text('{"biomes":[]}', encoding="utf-8")

    def test_external_biome_table_changes_both_tool_identities(self):
        with tempfile.TemporaryDirectory() as temp:
            roots = (Path(temp) / "first", Path(temp) / "second")
            for root in roots:
                self.write_identity_fixture(root)
            tools = (capture.capture_fixture_hash, comparison.identity)
            before = [tool(roots[0]) for tool in tools]
            table = roots[1] / "data/extra/biomes.json"
            table.write_text('{"biomes":[{"id":"different"}]}', encoding="utf-8")
            after = [tool(roots[1]) for tool in tools]
            for name, baseline, changed in zip(("capture", "comparison"), before, after):
                with self.subTest(tool=name):
                    self.assertNotEqual(baseline, changed)
            self.assertEqual(before[0], before[1])
            self.assertEqual(after[0], after[1])
            table.write_text('{"biomes":[]}', encoding="utf-8")
            self.assertEqual(before, [tool(roots[1]) for tool in tools])

    def test_controlled_marker_presence_and_content_affect_both_identities(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.write_identity_fixture(root)
            marker = root / comparison.MARKER
            tools = (capture.capture_fixture_hash, comparison.identity)
            controlled = [tool(root) for tool in tools]
            marker.write_text('{"gates":false}\n', encoding="utf-8")
            configured = [tool(root) for tool in tools]
            marker.unlink()
            shipping = [tool(root) for tool in tools]
            for identities in (controlled, configured, shipping):
                self.assertEqual(identities[0], identities[1])
            self.assertEqual(len({controlled[0], configured[0], shipping[0]}), 3)

    def test_inputs_match_and_saves_do_not_change_identity(self):
        with tempfile.TemporaryDirectory() as temp:
            base, head, receipt = self.write_preparation_fixture(Path(temp))
            common = base / "data/common"
            original = (common / "systems.json").read_bytes()
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

    def test_invalid_preset_leaves_tree_unchanged_and_retry_succeeds(self):
        for preset in capture.PRESETS:
            for invalid in (None, b'{', b'{"schema_rev":6,"schema_rev":7}',
                            b'{"generation_params":{"biomes":{"table":0}}}'):
                with self.subTest(preset=preset, invalid=invalid), tempfile.TemporaryDirectory() as temp:
                    root = Path(temp)
                    base, head, receipt = self.write_preparation_fixture(root)
                    path = base / "worlds/atlas/presets" / f"{preset}.json"
                    original = path.read_bytes()
                    if invalid is None:
                        path.unlink()
                    else:
                        path.write_bytes(invalid)
                    before = self.snapshot(root)
                    with self.assertRaises((ValueError, OSError)):
                        comparison.prepare(base, head, receipt)
                    self.assertEqual(before, self.snapshot(root))
                    path.write_bytes(original)
                    comparison.prepare(base, head, receipt)
                    runs = json.loads(receipt.read_text())["runs"]
                    self.assertEqual(len(runs), 2)
                    for checkout, run in zip((base, head), runs):
                        self.assertEqual(run["fixture_hash"], comparison.identity(checkout / "perf-common"))
                    self.assertEqual(runs[0]["fixture_hash"], runs[1]["fixture_hash"])

    def test_late_preparation_failure_rolls_back_both_checkouts(self):
        for failure in ("existing fixture", "head adapter", "receipt directory"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                base, head, receipt = self.write_preparation_fixture(root)
                source = base / "src/luminumbra_server/ServerWorldRunner.cpp"
                source.write_bytes(b'\xef\xbb\xbf' + source.read_bytes().replace(b'\n', b'\r\n'))
                if failure == "existing fixture":
                    fixture = head / "perf-common"
                    fixture.mkdir()
                    (fixture / "keep.json").write_text('{}')
                elif failure == "head adapter":
                    (head / "src/luminumbra_server/ServerWorldRunner.cpp").write_bytes(b'changed anchors\r\n')
                else:
                    receipt.mkdir()
                before = self.snapshot(root)
                with self.assertRaises((ValueError, OSError)):
                    comparison.prepare(base, head, receipt)
                self.assertEqual(before, self.snapshot(root))

    def test_receipt_write_failure_rolls_back_and_retry_succeeds(self):
        for mode in ("new", "existing", "nested"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                base, head, receipt = self.write_preparation_fixture(root)
                if mode == "existing":
                    receipt.write_bytes(b'previous receipt\r\n')
                elif mode == "nested":
                    receipt = root / "receipts/comparison/receipt.json"
                before = self.snapshot(root)
                write_text = Path.write_text

                def fail_receipt_write(path, *args, **kwargs):
                    if path == receipt:
                        path.write_bytes(b'partial receipt')
                        raise OSError("receipt write failed")
                    return write_text(path, *args, **kwargs)

                with mock.patch.object(Path, "write_text", fail_receipt_write):
                    with self.assertRaisesRegex(OSError, "receipt write failed"):
                        comparison.prepare(base, head, receipt)
                self.assertEqual(before, self.snapshot(root))
                comparison.prepare(base, head, receipt)
                runs = json.loads(receipt.read_text())["runs"]
                self.assertEqual(len(runs), 2)
                self.assertEqual(runs[0]["fixture_hash"], runs[1]["fixture_hash"])


if __name__ == "__main__":
    unittest.main()
