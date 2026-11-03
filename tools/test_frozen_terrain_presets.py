"""Stdlib checks for the frozen revision-6 save compatibility data."""

import hashlib
import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
FROZEN = ROOT / "worlds/atlas/presets-rev6"
SHIPPING = ROOT / "worlds/atlas/presets"
FIXTURE = ROOT / "test/fixtures/saves/revision6-hills"


def preset_checksum(preset):
    """Identity of authored/resolved preset bytes, shared with fixture regeneration."""
    canonical = json.dumps(preset, sort_keys=True, separators=(",", ":")).encode()
    checksum = 14695981039346656037
    for byte in canonical:
        checksum = ((checksum ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return f"{checksum:016x}"


class FrozenTerrainPresetsTests(unittest.TestCase):
    def test_original_bytes_match_manifest(self):
        manifest = json.loads((FROZEN / "sha256.json").read_text())
        self.assertEqual(set(manifest), {p.name for p in SHIPPING.glob("*.json")})
        self.assertEqual(len(manifest), 8)
        self.assertEqual(set(manifest), {p.name for p in FROZEN.glob("*.json")} -
                         {"provenance.json", "sha256.json"})
        for name, expected in manifest.items():
            with self.subTest(preset=name):
                self.assertEqual(hashlib.sha256((FROZEN / name).read_bytes()).hexdigest(), expected)

    def test_per_file_source_commits_cover_the_frozen_catalog(self):
        provenance = json.loads((FROZEN / "provenance.json").read_text())
        manifest = json.loads((FROZEN / "sha256.json").read_text())
        self.assertEqual(provenance["source_directory"], "worlds/atlas/presets")
        self.assertEqual(provenance["schema_rev"], 6)
        self.assertEqual(set(provenance["source_commits"]), set(manifest))
        for name, commit in provenance["source_commits"].items():
            with self.subTest(preset=name):
                self.assertEqual(commit, "4591c7eb9361e12f69432cd6c133211e5ed66842"
                                 if name == "cave_dense.json" else
                                 "3b2c0677f45319600bf01ce564fde47489033dfd")

    def test_cave_dense_follows_caverns_migration_without_losing_density(self):
        old = json.loads((FROZEN / "cave_dense.json").read_text())
        new = json.loads((SHIPPING / "cave_dense.json").read_text())
        self.assertEqual(new["schema_rev"], 8)
        self.assertIs(new["generation_params"]["terrain"]["island_mask_enabled"], True)
        self.assertEqual(new["generation_params"]["terrain"]["hydro"]["iterations"], 4)
        self.assertIs(new["generation_params"]["biomes"]["relief_enabled"], True)
        self.assertIs(new["generation_params"]["features"]["cliffs_enabled"], True)
        # These are the only authored changes, including metadata and cave tuning.
        restored = json.loads(json.dumps(new))
        restored["schema_rev"] = 6
        self.assertEqual(restored["generation_params"]["terrain"].pop("surface_sampling_revision"), 2)
        restored["generation_params"]["terrain"]["island_mask_enabled"] = False
        restored["generation_params"]["terrain"]["hydro"]["iterations"] = 8
        restored["generation_params"]["biomes"]["relief_enabled"] = False
        restored["generation_params"]["features"]["cliffs_enabled"] = False
        self.assertEqual(restored, old)
        for directory in (FROZEN, SHIPPING):
            caverns = json.loads((directory / "caverns.json").read_text())
            dense = json.loads((directory / "cave_dense.json").read_text())
            features = dense["generation_params"]["features"]
            self.assertEqual(features["cave_threshold"], 0.54)
            self.assertEqual(features["surface_break_density"], 0.55)
            self.assertEqual(features["max_feature_radius"], 52.0)
            for key in ("cave_threshold", "surface_break_density", "max_feature_radius"):
                features[key] = caverns["generation_params"]["features"][key]
            self.assertEqual(dense["generation_params"], caverns["generation_params"])

    def test_cave_dense_preset_identity_pins(self):
        for directory, expected in ((FROZEN, "e70158a4777ee36b"),
                                    (SHIPPING, "a9296ce03a5d9f33")):
            preset = json.loads((directory / "cave_dense.json").read_text())
            self.assertEqual(preset_checksum(preset), expected)

    def test_historical_and_new_world_revisions_stay_separate(self):
        for path in SHIPPING.glob("*.json"):
            with self.subTest(preset=path.name):
                self.assertEqual(json.loads(path.read_text())["schema_rev"], 8)
                self.assertEqual(json.loads(path.read_text())["generation_params"]["terrain"]["surface_sampling_revision"], 2)
                self.assertEqual(json.loads((FROZEN / path.name).read_text())["schema_rev"], 6)

    def test_old_hills_keeps_its_island_mask_off(self):
        old = json.loads((FROZEN / "default.json").read_text())
        new = json.loads((SHIPPING / "default.json").read_text())
        self.assertFalse(old["generation_params"]["terrain"]["island_mask_enabled"])
        self.assertTrue(new["generation_params"]["terrain"]["island_mask_enabled"])
        # Hills changed its island mask, schema and sampling revision; other old values survive.
        new["schema_rev"] = 6
        self.assertEqual(new["generation_params"]["terrain"].pop("surface_sampling_revision"), 2)
        new["generation_params"]["terrain"]["island_mask_enabled"] = False
        self.assertEqual(old, new)

    def test_flat_lands_keeps_its_minimal_generation_identity(self):
        old = json.loads((FROZEN / "flat_lands.json").read_text())["generation_params"]
        new = json.loads((SHIPPING / "flat_lands.json").read_text())["generation_params"]
        self.assertEqual(new["terrain"].pop("surface_sampling_revision"), 2)
        self.assertEqual(new["terrain"], old["terrain"])
        self.assertEqual(new["biomes"].pop("table"), "")
        self.assertIs(new["biomes"].pop("relief_enabled"), False)
        self.assertIs(new["features"].pop("surface_breaks_enabled"), False)
        self.assertEqual(new, old)

    def test_shipping_traversal_pins_the_current_preset(self):
        traversal = (ROOT / "tools/perf/fixtures/surface-flight.traversal").read_text()
        fields = dict(line.split(maxsplit=1) for line in traversal.splitlines()[1:])
        preset = json.loads((SHIPPING / (fields["preset"] + ".json")).read_text())
        self.assertEqual(int(fields["preset_revision"]), preset["schema_rev"])
        self.assertEqual(fields["preset_identity"], preset_checksum(preset))

    def test_non_embedded_fixture_pins_original_identity(self):
        metadata = json.loads((FIXTURE / "world_info.json").read_text())
        self.assertFalse((FIXTURE / "preset.json").exists())
        self.assertEqual(metadata["worldType"], "default")
        self.assertEqual(metadata["seed"], "424242")
        preset = json.loads((FROZEN / "default.json").read_text())
        self.assertEqual(metadata["presetIdentity"], preset_checksum(preset))
        self.assertEqual(metadata["presetIdentity"], "026f24207a8ffc7e")


if __name__ == "__main__":
    unittest.main()
