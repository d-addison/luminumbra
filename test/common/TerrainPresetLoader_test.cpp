// canonical TerrainPresetLoader unit coverage — consumed params,
// shaping, biome, and feature blocks plus unknown-key
// warnings, and the validation error contract.
#include "../support/UniqueTempPath.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/world/BiomeTable.h"
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
    EXPECT_TRUE(result.params.island_mask_enabled);
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
    EXPECT_FALSE(result.params.continental_spline.empty());
    EXPECT_TRUE(result.warnings.empty()) << result.warnings.front();
}

TEST(TerrainPresetLoaderTest, CanonicalResolvedPresetBytesAndHashAreStable) {
    using Luminumbra::world::CanonicalResolvedTerrainPresetJson;
    using Luminumbra::world::TerrainPresetContentHash;
    const auto preset = nlohmann::json::parse(kMinimalPreset);
    const auto equivalent = nlohmann::json::parse(R"({"generation_params":{
      "features":{"cave_frequency":0.02,"caves_enabled":true},
      "terrain":{"height_offset":20.0,"lacunarity":2.0,"persistence":0.5,
        "octaves":4,"base_amplitude":12.0,"base_frequency":0.01}},"name":"Minimal"})");
    const std::string expected =
        R"({"generation_params":{"biomes":{"relief_enabled":false,"table":""},"features":{"cave_frequency":0.02,"caves_enabled":true,"cliffs_enabled":false,"lakes_enabled":false,"rivers_enabled":false,"structures_enabled":false,"surface_breaks_enabled":false},"terrain":{"base_amplitude":12.0,"base_frequency":0.01,"height_offset":20.0,"hydro":{"enabled":false},"island_mask_enabled":false,"lacunarity":2.0,"octaves":4,"persistence":0.5}},"name":"Minimal","schema_rev":7})";
    EXPECT_EQ(CanonicalResolvedTerrainPresetJson(preset), expected);
    EXPECT_EQ(CanonicalResolvedTerrainPresetJson(equivalent), expected);
    EXPECT_EQ(TerrainPresetContentHash(preset), "24701fb4e837d18c");
    EXPECT_EQ(TerrainPresetContentHash(equivalent), "24701fb4e837d18c");
    // A persisted, revision-upgraded copy retains the same resolved content identity.
    EXPECT_EQ(CanonicalResolvedTerrainPresetJson(nlohmann::json::parse(expected)), expected);
    const auto first = LoadTerrainPresetFromJson(preset, "data-one", "first");
    const auto second = LoadTerrainPresetFromJson(equivalent, "data-two", "second");
    ASSERT_TRUE(first.ok);
    ASSERT_TRUE(second.ok);
    EXPECT_EQ(first.preset_content_hash, "24701fb4e837d18c");
    EXPECT_EQ(first.preset_content_hash, second.preset_content_hash);
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

TEST(TerrainPresetLoaderTest, ShippedPresetResolvesReadableBiomeTable) {
    const auto result = LoadTerrainPreset(PresetDir() / "default.json");
    ASSERT_TRUE(result.ok);
    ASSERT_TRUE(result.params.biomes_enabled);
    ASSERT_TRUE(Luminumbra::Filesystem::IsAbsolutePath(result.params.biome_table_path));
    std::ifstream input(result.params.biome_table_path, std::ios::binary);
    ASSERT_TRUE(input.is_open()) << result.params.biome_table_path;
    EXPECT_FALSE(Luminumbra::World::BiomeTable::Load(result.params.biome_table_path).empty());
}

TEST(TerrainPresetLoaderTest, AllShippedPresetsLoadWithoutErrorsOrWarnings) {
    for (const char* name : {"default",
                             "flat_lands",
                             "mountains",
                             "archipelago",
                             "temperate_forest",
                             "amplified",
                             "caverns",
                             "cave_dense"}) {
        std::ifstream input(PresetDir() / (std::string(name) + ".json"));
        ASSERT_TRUE(input.good()) << name;
        EXPECT_EQ(nlohmann::json::parse(input).at("schema_rev"), 7) << name;
        const TerrainPresetLoadResult result =
            LoadTerrainPreset(PresetDir() / (std::string(name) + ".json"));
        EXPECT_TRUE(result.ok) << name;
        EXPECT_TRUE(result.errors.empty()) << name << ": " << result.errors.front();
        EXPECT_TRUE(result.warnings.empty()) << name << ": " << result.warnings.front();
    }
}

TEST(TerrainPresetLoaderTest, CaveDensePreservesDensityAcrossRevisionSixAndSeven) {
    for (const int revision : {6, 7}) {
        SCOPED_TRACE(revision);
        const auto directory =
            revision == 6 ? PresetDir().parent_path() / "presets-rev6" : PresetDir();
        const auto result = LoadTerrainPreset(directory / "cave_dense.json");
        ASSERT_TRUE(result.ok);
        EXPECT_TRUE(result.errors.empty());
        EXPECT_TRUE(result.warnings.empty());
        const auto& params = result.params;
        const bool on = revision == 7;
        EXPECT_EQ(params.island_mask_enabled, on);
        EXPECT_EQ(params.biome_relief_enabled, on);
        EXPECT_EQ(params.cliffs_enabled, on);
        EXPECT_EQ(params.hydro_iterations, on ? 4 : 8);
        EXPECT_TRUE(params.hydro_enabled);
        EXPECT_TRUE(params.biomes_enabled);
        EXPECT_TRUE(params.caves_enabled);
        EXPECT_TRUE(params.surface_breaks_enabled);
        EXPECT_TRUE(params.rivers_enabled);
        EXPECT_TRUE(params.lakes_enabled);
        EXPECT_TRUE(params.structures_enabled);
        EXPECT_FLOAT_EQ(params.cave_threshold, 0.54f);
        EXPECT_FLOAT_EQ(params.surface_break_density, 0.55f);
        EXPECT_FLOAT_EQ(params.max_feature_radius, 52.0f);
    }
}

TEST(TerrainPresetLoaderTest, ParsesReservedShapingBlock) {
    const fs::path path = WriteTempPreset("luminumbra_shaping_preset.json", R"({
  "generation_params": {
    "terrain": {
      "base_frequency": 0.008, "base_amplitude": 120.0, "octaves": 6,
      "persistence": 0.65, "lacunarity": 2.2, "height_offset": 20.0,
      "shaping": {
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
    EXPECT_EQ(result.params.continental_spline, shaping.continental_spline);
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
    // Unversioned fixtures keep revision-6 absence semantics; resolving defaults
    // must not manufacture optional blocks or activate their shipping defaults.
    EXPECT_FALSE(result.extras.biomes.present);
    EXPECT_FALSE(result.extras.hydro.present);
    EXPECT_FALSE(result.params.biomes_enabled);
    EXPECT_FALSE(result.params.hydro_enabled);
    fs::remove(path);
}

TEST(TerrainPresetLoaderTest, LegacyAbsentBlocksStayOffWhenMaterializedForRevisionUpgrade) {
    auto resolved =
        Luminumbra::world::ResolveTerrainPresetDefaults(nlohmann::json::parse(kMinimalPreset));
    resolved["schema_rev"] = Luminumbra::world::kTerrainPresetSchemaRevision;
    const auto result = LoadTerrainPresetFromJson(resolved, {});
    ASSERT_TRUE(result.ok);
    EXPECT_FALSE(result.params.biomes_enabled);
    EXPECT_FALSE(result.params.biome_relief_enabled);
    EXPECT_FALSE(result.params.hydro_enabled);
    EXPECT_FALSE(result.params.island_mask_enabled);
    EXPECT_FALSE(result.params.rivers_enabled);
    EXPECT_FALSE(result.params.lakes_enabled);
    EXPECT_FALSE(result.params.cliffs_enabled);
    EXPECT_FALSE(result.params.surface_breaks_enabled);
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
// data-root-relative resolution is exercised independently of the shipped presets.
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
          "shaping": { "peaks_amplitude": 40.0 }
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
    EXPECT_EQ(mem.params.peaks_amplitude, disk.params.peaks_amplitude);
    EXPECT_EQ(mem.extras.shaping.present, disk.extras.shaping.present);
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

TEST(TerrainPresetLoaderTest, RejectsRetiredSelectorsRegardlessOfValueOrRevisionPresence) {
    for (bool explicit_revision : {false, true}) {
        for (const nlohmann::json& value : {nlohmann::json(false),
                                            nlohmann::json(true),
                                            nlohmann::json(0),
                                            nlohmann::json(1),
                                            nlohmann::json(nullptr),
                                            nlohmann::json("retired")}) {
            for (const char* selector : {"features.cave_style", "terrain.shaping.enabled"}) {
                auto preset = nlohmann::json::parse(kMinimalPreset);
                if (explicit_revision) {
                    preset["schema_rev"] = 6;
                }
                if (std::string(selector) == "features.cave_style") {
                    preset["generation_params"]["features"]["cave_style"] = value;
                } else {
                    preset["generation_params"]["terrain"]["shaping"]["enabled"] = value;
                }
                const auto result = LoadTerrainPresetFromJson(preset, {}, "retirement-test");
                EXPECT_FALSE(result.ok) << selector << "=" << value;
                ASSERT_FALSE(result.errors.empty());
                EXPECT_NE(result.errors.front().find(selector), std::string::npos);
                EXPECT_TRUE(result.warnings.empty());
            }
        }
    }
}

TEST(TerrainPresetLoaderTest, PreservesContentSwitchesAndOptionalErosionAndBiomes) {
    for (bool enabled : {false, true}) {
        auto preset = nlohmann::json::parse(kMinimalPreset);
        auto& gp = preset["generation_params"];
        gp["terrain"]["island_mask_enabled"] = enabled;
        gp["terrain"]["hydro"]["enabled"] = enabled;
        for (const char* key : {"caves_enabled",
                                "rivers_enabled",
                                "lakes_enabled",
                                "cliffs_enabled",
                                "structures_enabled"}) {
            gp["features"][key] = enabled;
        }
        gp["biomes"] = {{"relief_enabled", enabled}, {"relief_strength", 0.3}};
        if (enabled) {
            gp["biomes"]["table"] = "common/biomes.json";
        }
        const auto result = LoadTerrainPresetFromJson(preset, {}, "content-switches");
        ASSERT_TRUE(result.ok);
        EXPECT_EQ(result.params.island_mask_enabled, enabled);
        EXPECT_EQ(result.params.caves_enabled, enabled);
        EXPECT_EQ(result.params.rivers_enabled, enabled);
        EXPECT_EQ(result.params.lakes_enabled, enabled);
        EXPECT_EQ(result.params.cliffs_enabled, enabled);
        EXPECT_EQ(result.params.structures_enabled, enabled);
        EXPECT_EQ(result.params.hydro_enabled, enabled);
        EXPECT_EQ(result.params.biomes_enabled, enabled);
        EXPECT_EQ(result.params.biome_relief_enabled, enabled);
        EXPECT_FLOAT_EQ(result.extras.biomes.relief_strength, 0.3f);
        EXPECT_FLOAT_EQ(result.params.biome_relief_strength, enabled ? 0.3f : 0.45f);
    }
    for (const char* name : {"flat_lands", "temperate_forest"}) {
        const auto result = LoadTerrainPreset(PresetDir() / (std::string(name) + ".json"));
        ASSERT_TRUE(result.ok);
        const bool enabled = std::string(name) == "temperate_forest";
        EXPECT_EQ(result.params.hydro_enabled, enabled) << name;
        EXPECT_EQ(result.params.biomes_enabled, enabled) << name;
    }
    const auto archipelago = LoadTerrainPreset(PresetDir() / "archipelago.json");
    ASSERT_TRUE(archipelago.ok);
    EXPECT_TRUE(archipelago.params.biomes_enabled);
}

TEST(TerrainPresetLoaderTest, ExplicitOverridePreservesHistoricalTerrain) {
    const auto root = PresetDir().parent_path().parent_path().parent_path();
    const auto path = root / "worlds/atlas/presets-rev6/default.json";
    std::string text;
    std::string error;
    ASSERT_TRUE(Luminumbra::world::LoadTerrainPresetOverride(path, root / "data", text, error))
        << error;
    EXPECT_TRUE(error.empty());
    const auto document = nlohmann::json::parse(text);
    EXPECT_EQ(document["schema_rev"], 6);
    const auto historical = LoadTerrainPresetFromJson(document, root / "data");
    ASSERT_TRUE(historical.ok);
    EXPECT_FALSE(historical.params.island_mask_enabled);
    EXPECT_EQ(historical.params.biome_table_path, LoadTerrainPreset(path).params.biome_table_path);
    EXPECT_NE(historical.params.island_mask_enabled,
              LoadTerrainPreset(PresetDir() / "default.json").params.island_mask_enabled);
}

TEST(TerrainPresetLoaderTest, ExplicitOverrideRefusesInvalidFilesWithoutChangingText) {
    const auto root = PresetDir().parent_path().parent_path().parent_path();
    const auto directory =
        Luminumbra::TestSupport::CreateUniqueTempDirectory("lumin-preset-override-");
    const auto path = directory / "override.json";
    std::error_code remove_error;
    std::string text = "unchanged";
    std::string error;
    EXPECT_FALSE(Luminumbra::world::LoadTerrainPresetOverride(path, root / "data", text, error));
    EXPECT_EQ(text, "unchanged");
    EXPECT_FALSE(error.empty());
    for (const char* invalid : {"{", "[]", "{}", "{} trailing"}) {
        SCOPED_TRACE(invalid);
        {
            std::ofstream file(path);
            ASSERT_TRUE(file.good());
            file << invalid;
        }
        EXPECT_FALSE(
            Luminumbra::world::LoadTerrainPresetOverride(path, root / "data", text, error));
        EXPECT_EQ(text, "unchanged");
        EXPECT_FALSE(error.empty());
    }
    fs::remove_all(directory, remove_error);
}

} // namespace

TEST(TerrainPresetLoaderTest, RevisionSevenDefaultsAndSixCompatibility) {
    for (int revision : {6, 7}) {
        auto preset = nlohmann::json::parse(kMinimalPreset);
        preset["schema_rev"] = revision;
        const auto parsed = LoadTerrainPresetFromJson(preset, PresetDir() / "../../../data");
        ASSERT_TRUE(parsed.ok);
        const bool on = revision == 7;
        EXPECT_EQ(parsed.params.island_mask_enabled, on);
        EXPECT_EQ(parsed.params.surface_breaks_enabled, on);
        EXPECT_EQ(parsed.params.biomes_enabled, on);
        EXPECT_EQ(parsed.params.biome_relief_enabled, on);
        EXPECT_EQ(parsed.params.rivers_enabled, on);
        EXPECT_EQ(parsed.params.lakes_enabled, on);
        EXPECT_EQ(parsed.params.cliffs_enabled, on);
        EXPECT_EQ(parsed.params.structures_enabled, on);
        EXPECT_EQ(parsed.params.hydro_enabled, on);
        auto saved = Luminumbra::world::ResolveTerrainPresetDefaults(preset);
        saved["schema_rev"] = 7;
        const auto reopened = LoadTerrainPresetFromJson(saved, {});
        ASSERT_TRUE(reopened.ok);
        EXPECT_EQ(reopened.params.island_mask_enabled, on);
        EXPECT_EQ(reopened.params.surface_breaks_enabled, on);
        EXPECT_EQ(reopened.params.biomes_enabled, on);
        EXPECT_EQ(reopened.params.biome_relief_enabled, on);
        EXPECT_EQ(reopened.params.rivers_enabled, on);
        EXPECT_EQ(reopened.params.lakes_enabled, on);
        EXPECT_EQ(reopened.params.cliffs_enabled, on);
        EXPECT_EQ(reopened.params.structures_enabled, on);
        EXPECT_EQ(reopened.params.hydro_enabled, on);

        preset["generation_params"]["biomes"]["table"] = "";
        preset["generation_params"]["terrain"]["island_mask_enabled"] = false;
        preset["generation_params"]["terrain"]["hydro"]["enabled"] = false;
        for (const char* key : {"surface_breaks_enabled",
                                "rivers_enabled",
                                "lakes_enabled",
                                "cliffs_enabled",
                                "structures_enabled"})
            preset["generation_params"]["features"][key] = false;
        const auto off = LoadTerrainPresetFromJson(preset, {});
        ASSERT_TRUE(off.ok);
        EXPECT_FALSE(off.params.biomes_enabled);
        EXPECT_TRUE(off.params.biome_table_path.empty());
        EXPECT_FALSE(off.params.island_mask_enabled);
        EXPECT_FALSE(off.params.hydro_enabled);
        EXPECT_FALSE(off.params.rivers_enabled);
        EXPECT_FALSE(off.params.lakes_enabled);
        EXPECT_FALSE(off.params.cliffs_enabled);
        EXPECT_FALSE(off.params.structures_enabled);
        EXPECT_FALSE(off.params.surface_breaks_enabled);
    }
}

TEST(TerrainPresetLoaderTest, MalformedDefaultedBlocksReportContractErrors) {
    for (int revision : {6, 7}) {
        for (const char* field :
             {"/generation_params/terrain/hydro", "/generation_params/biomes"}) {
            for (const auto& invalid : {nlohmann::json(nullptr),
                                        nlohmann::json::array(),
                                        nlohmann::json(42),
                                        nlohmann::json("invalid"),
                                        nlohmann::json(false)}) {
                SCOPED_TRACE(revision);
                SCOPED_TRACE(field);
                SCOPED_TRACE(invalid.dump());
                auto preset = nlohmann::json::parse(kMinimalPreset);
                preset["schema_rev"] = revision;
                const nlohmann::json::json_pointer pointer(field);
                preset[pointer] = invalid;
                const auto resolved = Luminumbra::world::ResolveTerrainPresetDefaults(preset);
                EXPECT_EQ(resolved[pointer], invalid);

                const auto result = LoadTerrainPresetFromJson(preset, {}, "MalformedBlock");
                EXPECT_FALSE(result.ok);
                ASSERT_EQ(result.errors.size(), 1u);
                const std::string scope = std::string(field) == "/generation_params/terrain/hydro"
                                              ? "generation_params.terrain.hydro"
                                              : "generation_params.biomes";
                EXPECT_EQ(result.errors.front(),
                          "world preset field must be object: " + scope + ": MalformedBlock");
            }
        }
    }
}

TEST(TerrainPresetLoaderTest, MalformedDefaultedValuesReportContractErrors) {
    for (int revision : {6, 7}) {
        for (const char* field : {"/generation_params/terrain/island_mask_enabled",
                                  "/generation_params/terrain/hydro/enabled",
                                  "/generation_params/biomes/table",
                                  "/generation_params/biomes/relief_enabled",
                                  "/generation_params/features/rivers_enabled",
                                  "/generation_params/features/lakes_enabled",
                                  "/generation_params/features/cliffs_enabled",
                                  "/generation_params/features/structures_enabled",
                                  "/generation_params/features/surface_breaks_enabled"}) {
            for (const auto& invalid : {nlohmann::json(nullptr),
                                        nlohmann::json::array(),
                                        nlohmann::json(42),
                                        nlohmann::json::object()}) {
                SCOPED_TRACE(revision);
                SCOPED_TRACE(field);
                SCOPED_TRACE(invalid.dump());
                auto preset = nlohmann::json::parse(kMinimalPreset);
                preset["schema_rev"] = revision;
                const nlohmann::json::json_pointer pointer(field);
                preset[pointer] = invalid;
                const auto resolved = Luminumbra::world::ResolveTerrainPresetDefaults(preset);
                EXPECT_EQ(resolved[pointer], invalid);

                const auto result = LoadTerrainPresetFromJson(preset, {}, "MalformedValue");
                EXPECT_FALSE(result.ok);
                ASSERT_EQ(result.errors.size(), 1u);
                EXPECT_NE(result.errors.front().find("world preset field must be "),
                          std::string::npos);
                std::string scope(field + 1);
                for (char& character : scope) {
                    if (character == '/')
                        character = '.';
                }
                EXPECT_NE(result.errors.front().find(scope + ": MalformedValue"),
                          std::string::npos);
            }
        }
    }
}

TEST(TerrainPresetLoaderTest, AbsentOptionalBlocksAndFieldsReceiveVersionedDefaults) {
    for (int revision : {6, 7}) {
        for (bool empty_blocks : {false, true}) {
            SCOPED_TRACE(revision);
            SCOPED_TRACE(empty_blocks);
            auto preset = nlohmann::json::parse(kMinimalPreset);
            preset["schema_rev"] = revision;
            if (empty_blocks) {
                preset["generation_params"]["terrain"]["hydro"] = nlohmann::json::object();
                preset["generation_params"]["biomes"] = nlohmann::json::object();
            }
            const auto result = LoadTerrainPresetFromJson(preset, {});
            ASSERT_TRUE(result.ok);
            EXPECT_TRUE(result.errors.empty());
            const bool on = revision == 7;
            EXPECT_EQ(result.params.hydro_enabled, on);
            EXPECT_EQ(result.params.biomes_enabled, on);
            EXPECT_EQ(result.params.biome_relief_enabled, on);
            EXPECT_EQ(result.extras.biomes.table, on ? "common/biomes.json" : "");
            EXPECT_EQ(result.params.island_mask_enabled, on);
            EXPECT_EQ(result.params.rivers_enabled, on);
            EXPECT_EQ(result.params.lakes_enabled, on);
            EXPECT_EQ(result.params.cliffs_enabled, on);
            EXPECT_EQ(result.params.structures_enabled, on);
            EXPECT_EQ(result.params.surface_breaks_enabled, on);
        }
    }
}
