// canonical TerrainPresetLoader unit coverage — consumed params,
// shaping, biome, and feature blocks plus unknown-key
// warnings, and the validation error contract.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "luminumbra_common/world/TerrainPresetLoader.h"

namespace fs = std::filesystem;

namespace {

using Luminumbra::world::LoadTerrainPreset;
using Luminumbra::world::LoadTerrainPresetFromJson;
using Luminumbra::world::TerrainPresetLoadResult;

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

fs::path PresetDir() {
    return fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds" / "atlas" / "presets";
}

fs::path WriteTempPreset(const std::string& name, const std::string& contents) {
    const fs::path path = fs::temp_directory_path() / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << contents;
    return path;
}

constexpr const char* kMinimalPreset = R"({
  "name": "Minimal",
  "generation_params": {
    "terrain": {
      "base_frequency": 0.01,
      "base_amplitude": 12.0,
      "octaves": 4,
      "persistence": 0.5,
      "lacunarity": 2.0,
      "height_offset": 20.0
    },
    "features": {
      "caves_enabled": true,
      "cave_frequency": 0.02
    }
  }
})";

TEST(TerrainPresetLoaderTest, LoadsShippedDefaultPreset) {
    const TerrainPresetLoadResult result = LoadTerrainPreset(PresetDir() / "default.json");
    ASSERT_TRUE(result.ok) << (result.errors.empty() ? "" : result.errors.front());

    // DELIBERATE preset bump: default.json was
    // recalibrated against real-world DEM statistics (foothills class) and
    // gained a shaping block. Before: base_amplitude 12, octaves 4, no shaping.
    // After: base_amplitude 34, octaves 6, shaping present.
    EXPECT_FLOAT_EQ(result.params.base_frequency, 0.01f);
    EXPECT_FLOAT_EQ(result.params.base_amplitude, 34.0f);
    EXPECT_EQ(result.params.octaves, 6);
    EXPECT_FLOAT_EQ(result.params.persistence, 0.5f);
    EXPECT_FLOAT_EQ(result.params.lacunarity, 2.0f);
    EXPECT_FLOAT_EQ(result.params.height_offset, 5.0f);
    EXPECT_FALSE(result.params.island_mask_enabled);
    EXPECT_TRUE(result.params.caves_enabled);
    EXPECT_FLOAT_EQ(result.params.cave_frequency, 0.02f);
    // Unconsumed cave fields keep the engine defaults when absent.
    EXPECT_FLOAT_EQ(result.params.cave_threshold, 0.7f);
    EXPECT_FLOAT_EQ(result.params.cave_carve_value, 2.0f);

    EXPECT_TRUE(result.extras.biomes.present);
    EXPECT_FLOAT_EQ(result.extras.biomes.temperature_frequency, 0.005f);
    // Default was ENRICHED (worldgen "enrich default" slice): it now ships a biome table, rivers,
    // hydraulic erosion, lakes, cliffs and per-biome relief, so these are all consumed/enabled
    // (the prior monochrome default had them off). This test pins the currently shipped preset.
    EXPECT_TRUE(result.extras.biomes.enabled);
    EXPECT_TRUE(result.params.biomes_enabled);
    EXPECT_FALSE(result.params.biome_table_path.empty());
    EXPECT_TRUE(result.params.biome_relief_enabled);
    EXPECT_TRUE(result.extras.features.present);
    EXPECT_TRUE(result.extras.features.rivers_enabled);
    EXPECT_TRUE(result.params.rivers_enabled);
    EXPECT_TRUE(result.params.lakes_enabled);
    EXPECT_TRUE(result.params.cliffs_enabled);
    EXPECT_TRUE(result.params.hydro_enabled);
    EXPECT_TRUE(result.extras.features.structures_enabled);
    // Default now ships a shaping block ( DEM realism calibration).
    EXPECT_TRUE(result.extras.shaping.present);
    EXPECT_TRUE(result.params.shaping_enabled);
    EXPECT_TRUE(result.warnings.empty()) << result.warnings.front();
}

TEST(TerrainPresetLoaderTest, MountainsOptsIntoBiomesAndRivers) {
    // /2/3: mountains is the showcase preset - it opts into the biome
    // table (consumed) and into rivers (consumed). Loading it must warn on
    // nothing (every key is recognized).
    const TerrainPresetLoadResult result = LoadTerrainPreset(PresetDir() / "mountains.json");
    ASSERT_TRUE(result.ok) << (result.errors.empty() ? "" : result.errors.front());
    EXPECT_TRUE(result.warnings.empty())
        << (result.warnings.empty() ? "" : result.warnings.front());

    ASSERT_TRUE(result.extras.biomes.present);
    EXPECT_TRUE(result.extras.biomes.enabled);
    EXPECT_EQ(result.extras.biomes.table, "common/biomes.json");
    EXPECT_TRUE(result.params.biomes_enabled);
    EXPECT_FALSE(result.params.biome_table_path.empty());

    EXPECT_TRUE(result.extras.features.rivers_enabled);
    EXPECT_TRUE(result.params.rivers_enabled);
    EXPECT_FLOAT_EQ(result.params.river_pv_min, -1.0f);
    EXPECT_FLOAT_EQ(result.params.river_pv_max, -0.82f);
    EXPECT_FLOAT_EQ(result.params.river_depth, 4.0f);
}

TEST(TerrainPresetLoaderTest, AllShippedPresetsLoadWithoutErrorsOrWarnings) {
    for (const char* name :
         {"default", "flat_lands", "mountains", "archipelago", "temperate_forest"}) {
        const TerrainPresetLoadResult result =
            LoadTerrainPreset(PresetDir() / (std::string(name) + ".json"));
        EXPECT_TRUE(result.ok) << name;
        EXPECT_TRUE(result.errors.empty()) << name << ": " << result.errors.front();
        EXPECT_TRUE(result.warnings.empty()) << name << ": " << result.warnings.front();
    }
}

TEST(TerrainPresetLoaderTest, ParsesReservedShapingBlock) {
    const fs::path path = WriteTempPreset("luminumbra_shaping_preset.json", R"({
  "generation_params": {
    "terrain": {
      "base_frequency": 0.008, "base_amplitude": 120.0, "octaves": 6,
      "persistence": 0.65, "lacunarity": 2.2, "height_offset": 20.0,
      "shaping": {
        "enabled": true,
        "continentalness_frequency": 0.0008,
        "erosion_frequency": 0.0015,
        "peaks_frequency": 0.004,
        "peaks_amplitude": 90.0,
        "domain_warp_amplitude": 30.0,
        "domain_warp_frequency": 0.006,
        "continental_spline": [[-1.0, -40], [0.3, 14], [1.0, 42]],
        "erosion_spline": [[-1.0, 1.0], [1.0, 0.05]],
        "peaks_spline": [[-1.0, 0.0], [1.0, 1.0]]
      }
    },
    "features": { "caves_enabled": true, "cave_frequency": 0.03 }
  }
})");
    const TerrainPresetLoadResult result = LoadTerrainPreset(path);
    ASSERT_TRUE(result.ok) << (result.errors.empty() ? "" : result.errors.front());
    EXPECT_TRUE(result.warnings.empty()) << result.warnings.front();

    const auto& shaping = result.extras.shaping;
    ASSERT_TRUE(shaping.present);
    EXPECT_TRUE(shaping.enabled);
    EXPECT_FLOAT_EQ(shaping.continentalness_frequency, 0.0008f);
    EXPECT_FLOAT_EQ(shaping.erosion_frequency, 0.0015f);
    EXPECT_FLOAT_EQ(shaping.peaks_frequency, 0.004f);
    EXPECT_FLOAT_EQ(shaping.peaks_amplitude, 90.0f);
    EXPECT_FLOAT_EQ(shaping.domain_warp_amplitude, 30.0f);
    EXPECT_FLOAT_EQ(shaping.domain_warp_frequency, 0.006f);
    ASSERT_EQ(shaping.continental_spline.size(), 3u);
    EXPECT_FLOAT_EQ(shaping.continental_spline[0][0], -1.0f);
    EXPECT_FLOAT_EQ(shaping.continental_spline[0][1], -40.0f);
    ASSERT_EQ(shaping.erosion_spline.size(), 2u);
    ASSERT_EQ(shaping.peaks_spline.size(), 2u);

    fs::remove(path);
}

TEST(TerrainPresetLoaderTest, WarnsOnUnknownKeys) {
    const fs::path path = WriteTempPreset("luminumbra_unknown_keys_preset.json", R"({
  "name": "Unknown",
  "mystery_top": 1,
  "generation_params": {
    "terrain": {
      "base_frequency": 0.01, "base_amplitude": 12.0, "octaves": 4,
      "persistence": 0.5, "lacunarity": 2.0, "height_offset": 20.0,
      "mystery_terrain": true
    },
    "features": { "caves_enabled": true, "cave_frequency": 0.02, "mystery_feature": 3 },
    "mystery_block": {}
  }
})");
    const TerrainPresetLoadResult result = LoadTerrainPreset(path);
    ASSERT_TRUE(result.ok);
    ASSERT_EQ(result.warnings.size(), 4u);

    bool top = false, block = false, terrain = false, feature = false;
    for (const std::string& warning : result.warnings) {
        top = top || warning.find("$.mystery_top") != std::string::npos;
        block = block || warning.find("generation_params.mystery_block") != std::string::npos;
        terrain = terrain ||
                  warning.find("generation_params.terrain.mystery_terrain") != std::string::npos;
        feature = feature ||
                  warning.find("generation_params.features.mystery_feature") != std::string::npos;
    }
    EXPECT_TRUE(top);
    EXPECT_TRUE(block);
    EXPECT_TRUE(terrain);
    EXPECT_TRUE(feature);

    fs::remove(path);
}

TEST(TerrainPresetLoaderTest, MinimalPresetLoadsCleanly) {
    const fs::path path = WriteTempPreset("luminumbra_minimal_preset.json", kMinimalPreset);
    const TerrainPresetLoadResult result = LoadTerrainPreset(path);
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(result.warnings.empty());
    EXPECT_FALSE(result.extras.biomes.present);
    fs::remove(path);
}

TEST(TerrainPresetLoaderTest, MissingFileReportsOpenError) {
    const TerrainPresetLoadResult result = LoadTerrainPreset(PresetDir() / "does_not_exist.json");
    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.errors.size(), 1u);
    EXPECT_NE(result.errors.front().find("failed to open world preset"), std::string::npos);
}

TEST(TerrainPresetLoaderTest, InvalidJsonReportsParseError) {
    const fs::path path = WriteTempPreset("luminumbra_invalid_preset.json", "{ not json");
    const TerrainPresetLoadResult result = LoadTerrainPreset(path);
    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.errors.size(), 1u);
    EXPECT_NE(result.errors.front().find("failed to parse world preset JSON"), std::string::npos);
    fs::remove(path);
}

TEST(TerrainPresetLoaderTest, MissingRequiredFieldsReportContractErrors) {
    const fs::path path = WriteTempPreset("luminumbra_missing_fields_preset.json", R"({
  "generation_params": {
    "terrain": { "base_frequency": "not-a-number" },
    "features": {}
  }
})");
    const TerrainPresetLoadResult result = LoadTerrainPreset(path);
    EXPECT_FALSE(result.ok);
    // 6 terrain numerics + caves_enabled + cave_frequency.
    EXPECT_EQ(result.errors.size(), 8u);
    fs::remove(path);
}

TEST(TerrainPresetLoaderTest, MissingGenerationParamsIsAnError) {
    const fs::path path =
        WriteTempPreset("luminumbra_no_genparams_preset.json", R"({ "name": "x" })");
    const TerrainPresetLoadResult result = LoadTerrainPreset(path);
    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.errors.size(), 1u);
    EXPECT_NE(result.errors.front().find("missing object generation_params"), std::string::npos);
    fs::remove(path);
}

//  the in-memory seam (LoadTerrainPresetFromJson) must produce
// the SAME params/extras as the on-disk loader for identical content — proving
// the file path simply delegates and the create-world live preview gets the same
// world the on-disk create would. Uses a FIXED literal with a biome table so the
// data-root-relative resolution is exercised (NOT default.json — owned by the
// concurrent worldgen agent).
TEST(TerrainPresetLoaderTest, InMemorySeamMatchesOnDiskLoad) {
    const std::string kPreset = R"({
      "name": "SeamParity",
      "generation_params": {
        "terrain": {
          "base_frequency": 0.013,
          "base_amplitude": 47.0,
          "octaves": 5,
          "persistence": 0.55,
          "lacunarity": 2.1,
          "height_offset": 9.0,
          "shaping": { "enabled": true, "peaks_amplitude": 40.0 }
        },
        "biomes": { "table": "common/biomes.json", "relief_enabled": true },
        "features": {
          "caves_enabled": true, "cave_frequency": 0.02,
          "rivers_enabled": true, "structures_enabled": true,
          "cliffs_enabled": true
        }
      }
    })";

    // On-disk: write into the real preset dir tree so the four-parents-up data
    // root resolves to <source_root>/data (where common/biomes.json lives).
    const fs::path disk_path = PresetDir() / "luminumbra_seam_parity_preset.json";
    {
        std::ofstream out(disk_path, std::ios::binary | std::ios::trunc);
        out << kPreset;
    }
    const TerrainPresetLoadResult disk = LoadTerrainPreset(disk_path);
    fs::remove(disk_path);
    ASSERT_TRUE(disk.ok) << (disk.errors.empty() ? "" : disk.errors.front());

    // In-memory: same content, explicit data root = <source_root>/data.
    const fs::path data_root = fs::path(LUMINUMBRA_SOURCE_ROOT) / "data";
    const nlohmann::json parsed = nlohmann::json::parse(kPreset);
    const TerrainPresetLoadResult mem =
        LoadTerrainPresetFromJson(parsed, data_root, "<seam-parity>");
    ASSERT_TRUE(mem.ok) << (mem.errors.empty() ? "" : mem.errors.front());

    // Consumed params parity (the bytes the world system + world_hash consume).
    EXPECT_FLOAT_EQ(mem.params.base_frequency, disk.params.base_frequency);
    EXPECT_FLOAT_EQ(mem.params.base_amplitude, disk.params.base_amplitude);
    EXPECT_EQ(mem.params.octaves, disk.params.octaves);
    EXPECT_FLOAT_EQ(mem.params.height_offset, disk.params.height_offset);
    EXPECT_EQ(mem.params.shaping_enabled, disk.params.shaping_enabled);
    EXPECT_EQ(mem.params.biomes_enabled, disk.params.biomes_enabled);
    EXPECT_EQ(mem.params.biome_table_path, disk.params.biome_table_path);
    EXPECT_EQ(mem.params.biome_relief_enabled, disk.params.biome_relief_enabled);
    EXPECT_EQ(mem.params.rivers_enabled, disk.params.rivers_enabled);
    EXPECT_EQ(mem.params.cliffs_enabled, disk.params.cliffs_enabled);
    EXPECT_EQ(mem.params.structures_enabled, disk.params.structures_enabled);
    EXPECT_EQ(mem.params.structures_data_dir, disk.params.structures_data_dir);
    // The resolved table path must be a real absolute path into the data root.
    EXPECT_FALSE(mem.params.biome_table_path.empty());
    EXPECT_NE(mem.params.biome_table_path.find("biomes.json"), std::string::npos);
}

} // namespace
