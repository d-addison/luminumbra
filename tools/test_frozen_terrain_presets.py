"""Stdlib checks for the frozen revision-6 save compatibility data."""

import hashlib
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
FROZEN = ROOT / "worlds/atlas/presets-rev6"
SHIPPING = ROOT / "worlds/atlas/presets"
FIXTURE = ROOT / "test/fixtures/saves/revision6-hills"


class FrozenTerrainPresetsTests(unittest.TestCase):
    def test_original_bytes_match_manifest(self):
        manifest = json.loads((FROZEN / "sha256.json").read_text())
        self.assertEqual(set(manifest), {p.name for p in SHIPPING.glob("*.json")})
        self.assertEqual(len(manifest), 7)
        for name, expected in manifest.items():
            with self.subTest(preset=name):
                self.assertEqual(hashlib.sha256((FROZEN / name).read_bytes()).hexdigest(), expected)

    def test_historical_and_new_world_revisions_stay_separate(self):
        for path in SHIPPING.glob("*.json"):
            with self.subTest(preset=path.name):
                self.assertEqual(json.loads(path.read_text())["schema_rev"], 7)
                self.assertEqual(json.loads((FROZEN / path.name).read_text())["schema_rev"], 6)

    def test_old_hills_keeps_its_island_mask_off(self):
        old = json.loads((FROZEN / "default.json").read_text())
        new = json.loads((SHIPPING / "default.json").read_text())
        self.assertFalse(old["generation_params"]["terrain"]["island_mask_enabled"])
        self.assertTrue(new["generation_params"]["terrain"]["island_mask_enabled"])
        # Hills changed only its island mask and revision; every other old value survives.
        new["schema_rev"] = 6
        new["generation_params"]["terrain"]["island_mask_enabled"] = False
        self.assertEqual(old, new)

    def test_flat_lands_keeps_its_minimal_generation_identity(self):
        old = json.loads((FROZEN / "flat_lands.json").read_text())["generation_params"]
        new = json.loads((SHIPPING / "flat_lands.json").read_text())["generation_params"]
        self.assertEqual(new["terrain"], old["terrain"])
        self.assertEqual(new["biomes"].pop("table"), "")
        self.assertIs(new["biomes"].pop("relief_enabled"), False)
        self.assertIs(new["features"].pop("surface_breaks_enabled"), False)
        self.assertEqual(new, old)

    def test_shipping_traversal_pins_the_current_preset(self):
        traversal = (ROOT / "tools/perf/fixtures/surface-flight.traversal").read_text()
        fields = dict(line.split(maxsplit=1) for line in traversal.splitlines()[1:])
        preset = json.loads((SHIPPING / (fields["preset"] + ".json")).read_text())
        canonical = json.dumps(preset, sort_keys=True, separators=(",", ":")).encode()
        checksum = 14695981039346656037
        for byte in canonical:
            checksum = ((checksum ^ byte) * 1099511628211) & ((1 << 64) - 1)
        self.assertEqual(int(fields["preset_revision"]), preset["schema_rev"])
        self.assertEqual(fields["preset_identity"], f"{checksum:016x}")

    def test_non_embedded_fixture_pins_original_identity(self):
        metadata = json.loads((FIXTURE / "world_info.json").read_text())
        self.assertFalse((FIXTURE / "preset.json").exists())
        self.assertEqual(metadata["worldType"], "default")
        self.assertEqual(metadata["seed"], "424242")
        preset = json.loads((FROZEN / "default.json").read_text())
        canonical = json.dumps(preset, sort_keys=True, separators=(",", ":")).encode()
        checksum = 14695981039346656037
        for byte in canonical:
            checksum = ((checksum ^ byte) * 1099511628211) & ((1 << 64) - 1)
        self.assertEqual(metadata["presetIdentity"], f"{checksum:016x}")
        self.assertEqual(metadata["presetIdentity"], "026f24207a8ffc7e")


if __name__ == "__main__":
    unittest.main()
