// canonical TerrainPresetLoader unit coverage — consumed params,
// shaping, biome, and feature blocks plus unknown-key
// warnings, and the validation error contract.
#include "../support/UniqueTempPath.h"
#include "../support/WorldgenTestData.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <map>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

#include "luminumbra/core/Types.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/BiomeTable.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/StructurePlacement.h"
#include "luminumbra_common/world/TerrainPresetLoader.h"
#include "luminumbra_common/world/WorldgenFileReader.h"

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
    EXPECT_EQ(result.params.surface_sampling_revision, 2);

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

TEST(TerrainPresetLoaderTest, SurfaceSamplingRevisionPreservesSavedChoiceAndIdentity) {
    const auto original = nlohmann::json::parse(kMinimalPreset);
    const auto legacy = LoadTerrainPresetFromJson(original, {});
    ASSERT_TRUE(legacy.ok);
    EXPECT_EQ(legacy.params.surface_sampling_revision, 1);
    EXPECT_FALSE(Luminumbra::world::ResolveTerrainPresetDefaults(original)
                     .at("generation_params")
                     .at("terrain")
                     .contains("surface_sampling_revision"));
    for (const int revision : {1, 2}) {
        auto saved = original;
        saved["generation_params"]["terrain"]["surface_sampling_revision"] = revision;
        const auto loaded = LoadTerrainPresetFromJson(
            Luminumbra::world::ResolveEffectivePreset(nlohmann::json::parse(saved.dump())), {});
        ASSERT_TRUE(loaded.ok);
        EXPECT_EQ(loaded.params.surface_sampling_revision, revision);
        if (revision == 2) {
            EXPECT_FALSE(LoadTerrainPresetFromJson(saved, {}).ok);
        }
        EXPECT_NE(loaded.preset_content_hash, legacy.preset_content_hash);
    }
    for (const auto& invalid : {nlohmann::json(0),
                                nlohmann::json(3),
                                nlohmann::json(-1),
                                nlohmann::json(2.0),
                                nlohmann::json(true),
                                nlohmann::json("2"),
                                nlohmann::json(nullptr),
                                nlohmann::json::array(),
                                nlohmann::json(18446744073709551615ull)}) {
        auto saved = original;
        saved["generation_params"]["terrain"]["surface_sampling_revision"] = invalid;
        const auto loaded = LoadTerrainPresetFromJson(saved, {});
        EXPECT_FALSE(loaded.ok) << invalid.dump();
        ASSERT_FALSE(loaded.errors.empty());
        EXPECT_NE(loaded.errors.front().find("surface_sampling_revision"), std::string::npos);
    }
}

TEST(TerrainPresetLoaderTest, EffectivePresetResolvesSelectorBeforeIdentityAndCompatibilityGate) {
    using Luminumbra::world::CanonicalResolvedTerrainPresetJson;
    using Luminumbra::world::ResolveEffectivePreset;
    using Luminumbra::world::TerrainPresetUse;
    const auto original = nlohmann::json::parse(kMinimalPreset);
    const auto historical = ResolveEffectivePreset(original);
    const auto current = ResolveEffectivePreset(original, TerrainPresetUse::NewWorld);
    EXPECT_FALSE(
        historical.at("generation_params").at("terrain").contains("surface_sampling_revision"));
    EXPECT_EQ(current.at("generation_params").at("terrain").at("surface_sampling_revision"), 2);
    EXPECT_EQ(current.at("schema_rev"), 8);
    // Simulate the enforced upper bound of the older preset reader. The saved
    // contract must be refused there, rather than accepted with unknown-key warnings.
    constexpr int historical_reader_max = 7;
    EXPECT_GT(nlohmann::json::parse(CanonicalResolvedTerrainPresetJson(current))
                  .at("schema_rev")
                  .get<int>(),
              historical_reader_max);
    EXPECT_EQ(ResolveEffectivePreset(current), current);
    EXPECT_EQ(ResolveEffectivePreset(current, TerrainPresetUse::NewWorld), current);
    EXPECT_NE(LoadTerrainPresetFromJson(current, {}).preset_content_hash,
              LoadTerrainPresetFromJson(historical, {}).preset_content_hash);
    std::array<std::string, 2> hashes;
    for (const int revision : {1, 2}) {
        auto explicit_preset = original;
        explicit_preset["generation_params"]["terrain"]["surface_sampling_revision"] = revision;
        const auto resolved = ResolveEffectivePreset(explicit_preset, TerrainPresetUse::NewWorld);
        const auto loaded = LoadTerrainPresetFromJson(resolved, {});
        ASSERT_TRUE(loaded.ok);
        EXPECT_EQ(loaded.params.surface_sampling_revision, revision);
        EXPECT_EQ(loaded.preset_content_hash,
                  LoadTerrainPresetFromJson(ResolveEffectivePreset(explicit_preset), {})
                      .preset_content_hash);
        hashes[static_cast<std::size_t>(revision - 1)] = loaded.preset_content_hash;
        const auto saved = nlohmann::json::parse(CanonicalResolvedTerrainPresetJson(resolved));
        EXPECT_EQ(saved.at("schema_rev"), revision == 1 ? 7 : 8);
        EXPECT_EQ(LoadTerrainPresetFromJson(saved, {}).preset_content_hash,
                  loaded.preset_content_hash);
    }
    EXPECT_NE(hashes[0], hashes[1]);
    for (const auto& schema : {nlohmann::json(5), nlohmann::json(9), nlohmann::json("8")}) {
        auto invalid = current;
        invalid["schema_rev"] = schema;
        const auto resolved = ResolveEffectivePreset(invalid, TerrainPresetUse::NewWorld);
        EXPECT_EQ(resolved.at("schema_rev"), schema);
        EXPECT_FALSE(LoadTerrainPresetFromJson(resolved, {}).ok);
    }
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
    EXPECT_EQ(TerrainPresetContentHash(preset), "5dd4531c44219c30");
    EXPECT_EQ(TerrainPresetContentHash(equivalent), "5dd4531c44219c30");
    // A persisted, revision-upgraded copy retains the same resolved content identity.
    EXPECT_EQ(CanonicalResolvedTerrainPresetJson(nlohmann::json::parse(expected)), expected);
    const auto first = LoadTerrainPresetFromJson(preset, "data-one", "first");
    const auto second = LoadTerrainPresetFromJson(equivalent, "data-two", "second");
    ASSERT_TRUE(first.ok);
    ASSERT_TRUE(second.ok);
    EXPECT_EQ(first.preset_content_hash, "5dd4531c44219c30");
    EXPECT_EQ(first.preset_content_hash, second.preset_content_hash);
}

TEST(TerrainPresetLoaderTest, DependencyBytesAreBoundIntoPresetIdentity) {
    const Luminumbra::TestSupport::ScopedTempFile temp("preset-dependency-bytes-", ".json");
    const auto root = temp.Path().parent_path();
    Luminumbra::TestSupport::CopyWorldgenTestData(LUMINUMBRA_SOURCE_ROOT, root);
    auto preset = nlohmann::json::parse(kMinimalPreset);
    preset["schema_rev"] = 7; // selects the biome table and all structure types by default
    const auto baseline = LoadTerrainPresetFromJson(preset, root / "data");
    ASSERT_TRUE(baseline.ok);
    ASSERT_FALSE(baseline.preset_content_hash.empty());
    EXPECT_NE(baseline.preset_content_hash, Luminumbra::world::TerrainPresetContentHash(preset));
    std::vector<fs::path> dependencies{root / "data/common/biomes.json"};
    for (const auto& entry : fs::recursive_directory_iterator(root / "data/common/structures")) {
        if (entry.is_regular_file() && entry.path().extension() == ".json")
            dependencies.push_back(entry.path());
    }
    ASSERT_GT(dependencies.size(), 1u);
    for (const auto& path : dependencies) {
        SCOPED_TRACE(Luminumbra::Filesystem::GenericUtf8PathString(path));
        const auto original_size = fs::file_size(path);
        {
            std::ofstream file(path, std::ios::binary | std::ios::app);
            ASSERT_TRUE(file.good());
            file << '\n'; // JSON semantics stay identical; the exact file bytes change.
            file.close();
            ASSERT_TRUE(file.good());
        }
        const auto changed = LoadTerrainPresetFromJson(preset, root / "data");
        ASSERT_TRUE(changed.ok);
        EXPECT_NE(changed.preset_content_hash, baseline.preset_content_hash);
        fs::resize_file(path, original_size);
        const auto restored = LoadTerrainPresetFromJson(preset, root / "data");
        ASSERT_TRUE(restored.ok);
        EXPECT_EQ(restored.preset_content_hash, baseline.preset_content_hash);
    }
}

TEST(TerrainPresetLoaderTest, SnapshotReaderSuppliesEveryDependencyAndRefusesPartialReads) {
    const Luminumbra::TestSupport::ScopedTempFile temp("preset-reader-", ".json");
    const auto root = temp.Path().parent_path();
    Luminumbra::TestSupport::CopyWorldgenTestData(LUMINUMBRA_SOURCE_ROOT, root);
    const auto data_root = root / "data";
    const auto species = data_root / "common/creatures/species/fixture.json";
    fs::create_directories(species.parent_path());
    {
        std::ofstream file(species, std::ios::binary);
        file << R"({"id":"fixture"})";
    }
    auto preset = nlohmann::json::parse(kMinimalPreset);
    preset["schema_rev"] = 7;
    const auto baseline = LoadTerrainPresetFromJson(preset, data_root);
    ASSERT_TRUE(baseline.ok);
    std::map<fs::path, std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(data_root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            std::string bytes;
            ASSERT_TRUE(Luminumbra::World::ReadWorldgenFile(entry.path(), bytes));
            files.emplace(entry.path(), std::move(bytes));
            std::ofstream file(entry.path(), std::ios::binary | std::ios::trunc);
            file << '{'; // Any fallback to disk must fail JSON parsing.
        }
    }
    std::map<fs::path, int> reads;
    const auto reader = [&](const fs::path& path, std::string& bytes) {
        ++reads[path];
        bytes = files.at(path);
        return true;
    };
    const auto load = [&](const Luminumbra::World::WorldgenFileReader& dependency_reader) {
        return LoadTerrainPresetFromJson(preset,
                                         data_root,
                                         "snapshot",
                                         Luminumbra::World::ReadWorldgenSymlinkStatus,
                                         dependency_reader);
    };
    const auto captured = load(reader);
    ASSERT_TRUE(captured.ok);
    ASSERT_TRUE(captured.warnings.empty());
    EXPECT_EQ(captured.preset_content_hash, baseline.preset_content_hash);
    EXPECT_EQ(reads.size(), files.size());
    for (const auto& [path, count] : reads) {
        SCOPED_TRACE(Luminumbra::Filesystem::GenericUtf8PathString(path));
        EXPECT_EQ(count, 1);
    }
    for (const auto& [path, unused] : files) {
        SCOPED_TRACE(Luminumbra::Filesystem::GenericUtf8PathString(path));
        const auto partial = load([&](const fs::path& requested, std::string& bytes) {
            if (requested == path) {
                bytes = "{";
                return false;
            }
            return reader(requested, bytes);
        });
        EXPECT_FALSE(partial.ok);
        EXPECT_TRUE(partial.preset_content_hash.empty());
        EXPECT_FALSE(partial.params.worldgen_dependencies);
    }
    const auto unknown = load([](const fs::path&, std::string&) -> bool {
        throw std::runtime_error("uncaptured dependency");
    });
    EXPECT_FALSE(unknown.ok);
    EXPECT_TRUE(unknown.preset_content_hash.empty());
    EXPECT_FALSE(unknown.params.worldgen_dependencies);
    // Species remain mandatory when terrain dependencies are disabled.
    preset["schema_rev"] = 6;
    reads.clear();
    ASSERT_TRUE(load(reader).ok);
    ASSERT_EQ(reads.size(), 1u);
    EXPECT_EQ(reads.begin()->first, species);
}

nlohmann::json HandoffPreset() {
    auto preset = nlohmann::json::parse(kMinimalPreset);
    preset["schema_rev"] = 7;
    auto& terrain = preset["generation_params"]["terrain"];
    terrain["base_amplitude"] = 4.0;
    terrain["height_offset"] = 40.0;
    terrain["island_mask_enabled"] = false;
    terrain["hydro"]["enabled"] = false;
    auto& features = preset["generation_params"]["features"];
    for (const char* feature : {"caves_enabled",
                                "rivers_enabled",
                                "lakes_enabled",
                                "cliffs_enabled",
                                "surface_breaks_enabled"})
        features[feature] = false;
    return preset;
}

void ExpectSameGeneration(const Luminumbra::Systems::SHIELD_WorldSystem& actual,
                          const Luminumbra::Systems::SHIELD_WorldSystem& expected,
                          const Luminumbra::IVec3& coords) {
    EXPECT_EQ(actual.biomes_enabled(), expected.biomes_enabled());
    EXPECT_EQ(actual.biome_table().content_hash(), expected.biome_table().content_hash());
    EXPECT_EQ(actual.structures_enabled(), expected.structures_enabled());
    ASSERT_EQ(actual.structure_pools().size(), expected.structure_pools().size());
    for (std::size_t i = 0; i < expected.structure_pools().size(); ++i) {
        const auto& a = actual.structure_pools()[i];
        const auto& b = expected.structure_pools()[i];
        EXPECT_EQ(a.type, b.type);
        EXPECT_EQ(a.content_hash, b.content_hash);
        const auto a_site = actual.LocateStructure(a.type, 0, 0);
        const auto b_site = expected.LocateStructure(b.type, 0, 0);
        ASSERT_EQ(a_site.has_value(), b_site.has_value());
        if (a_site.has_value() && b_site.has_value()) {
            EXPECT_EQ(a_site->origin, b_site->origin);
            EXPECT_EQ(a_site->site_seed, b_site->site_seed);
            EXPECT_EQ(Luminumbra::World::ComputeAssembledVoxelHash(
                          Luminumbra::World::AssembleStructure(a, *a_site)),
                      Luminumbra::World::ComputeAssembledVoxelHash(
                          Luminumbra::World::AssembleStructure(b, *b_site)));
        }
    }
    Luminumbra::Chunk a(coords), b(coords);
    actual.GenerateChunkData(a);
    expected.GenerateChunkData(b);
    EXPECT_EQ(a.sdf_data, b.sdf_data);
    EXPECT_EQ(a.heightmap_data, b.heightmap_data);
    EXPECT_EQ(a.material_data, b.material_data);
}

TEST(TerrainPresetLoaderTest, GenerationUsesLoadedSnapshotAfterDependencyMutationOrRemoval) {
    const Luminumbra::TestSupport::ScopedTempFile temp("preset-handoff-", ".json");
    const auto root = temp.Path().parent_path();
    const auto control_root = root / "control";
    Luminumbra::TestSupport::CopyWorldgenTestData(LUMINUMBRA_SOURCE_ROOT, control_root);
    const auto preset = HandoffPreset();
    const auto control = LoadTerrainPresetFromJson(preset, control_root / "data");
    ASSERT_TRUE(control.ok);
    ASSERT_TRUE(control.params.worldgen_dependencies);
    const Luminumbra::Systems::SHIELD_WorldSystem expected(nullptr, nullptr, control.params, 1337);
    ASSERT_TRUE(expected.biomes_enabled());
    ASSERT_TRUE(expected.structures_enabled());
    const auto site = expected.LocateStructure("cairn", 0, 0);
    ASSERT_TRUE(site.has_value());
    if (!site.has_value())
        return;
    const float surface = expected.GetTerrainHeightAt(static_cast<float>(site->origin.x),
                                                      static_cast<float>(site->origin.z));
    const auto coords =
        Luminumbra::Systems::SHIELD_WorldSystem::world_to_chunk_coords(Luminumbra::Vec3(
            static_cast<float>(site->origin.x), surface, static_cast<float>(site->origin.z)));
    Luminumbra::Chunk stamped(coords);
    expected.GenerateChunkData(stamped);
    ASSERT_FALSE(stamped.material_data.empty());

    const std::vector<fs::path> dependencies = {"common/biomes.json",
                                                "common/structures/cairn/placement.json",
                                                "common/structures/cairn/cairn.json",
                                                "common/structures"};
    for (std::size_t i = 0; i < dependencies.size(); ++i) {
        for (const bool remove : {false, true}) {
            SCOPED_TRACE(dependencies[i]);
            SCOPED_TRACE(remove);
            const auto changed_root = root / (std::to_string(i) + (remove ? "r" : "m"));
            Luminumbra::TestSupport::CopyWorldgenTestData(LUMINUMBRA_SOURCE_ROOT, changed_root);
            const auto loaded = LoadTerrainPresetFromJson(preset, changed_root / "data");
            ASSERT_TRUE(loaded.ok);
            ASSERT_TRUE(loaded.params.worldgen_dependencies);
            ASSERT_EQ(loaded.preset_content_hash, control.preset_content_hash);
            EXPECT_EQ(loaded.params.worldgen_dependencies->manifest,
                      control.params.worldgen_dependencies->manifest);
            const auto path = changed_root / "data" / dependencies[i];
            if (remove) {
                ASSERT_GT(fs::remove_all(path), 0u);
            } else if (i == dependencies.size() - 1) {
                const auto added = path / "added";
                fs::create_directory(added);
                std::ofstream out(added / "piece.json", std::ios::binary);
                out << R"({"boxes":[{"size":[1,1,1],"material":2}]})";
                out.close();
                ASSERT_TRUE(out.good());
            } else {
                // A semantic change, not just whitespace: reject the table/piece
                // or suppress all cairn placement if generation reloads the file.
                std::ofstream out(path, std::ios::binary | std::ios::trunc);
                out << (i == 1 ? R"({"density":0})" : R"({})");
                out.close();
                ASSERT_TRUE(out.good());
            }
            const auto reloaded = LoadTerrainPresetFromJson(preset, changed_root / "data");
            const bool refused = (remove && i >= 2) || (!remove && (i == 0 || i == 2));
            EXPECT_EQ(reloaded.ok, !refused);
            if (refused) {
                EXPECT_FALSE(reloaded.errors.empty());
                EXPECT_TRUE(reloaded.preset_content_hash.empty());
            } else {
                EXPECT_NE(reloaded.preset_content_hash, loaded.preset_content_hash);
            }
            const Luminumbra::Systems::SHIELD_WorldSystem actual(
                nullptr, nullptr, loaded.params, 1337);
            ExpectSameGeneration(actual, expected, coords);
            EXPECT_EQ(loaded.preset_content_hash, control.preset_content_hash);
        }
    }
}

TEST(TerrainPresetLoaderTest, MissingOrRejectedInputsRefuseUntilRepaired) {
    const Luminumbra::TestSupport::ScopedTempFile temp("preset-fallback-handoff-", ".json");
    const auto root = temp.Path().parent_path();
    const auto preset = HandoffPreset();
    const std::vector<fs::path> dependencies = {"common/biomes.json",
                                                "common/structures/cairn/placement.json",
                                                "common/structures/cairn/cairn.json",
                                                "common/structures"};
    for (std::size_t i = 0; i < dependencies.size(); ++i) {
        for (const bool absent : {false, true}) {
            SCOPED_TRACE(dependencies[i]);
            SCOPED_TRACE(absent);
            const auto case_root = root / (std::to_string(i) + (absent ? "a" : "r"));
            const auto control_root = case_root / "control";
            const auto changed_root = case_root / "changed";
            for (const auto& runtime_root : {control_root, changed_root}) {
                Luminumbra::TestSupport::CopyWorldgenTestData(LUMINUMBRA_SOURCE_ROOT, runtime_root);
                const auto path = runtime_root / "data" / dependencies[i];
                if (absent) {
                    ASSERT_GT(fs::remove_all(path), 0u);
                } else if (i == dependencies.size() - 1) {
                    ASSERT_GT(fs::remove_all(path), 0u);
                    fs::create_directory(path); // present but empty, then populated at handoff
                } else {
                    std::ofstream out(path, std::ios::binary | std::ios::trunc);
                    out << "{invalid";
                    out.close();
                    ASSERT_TRUE(out.good());
                }
            }
            const auto control = LoadTerrainPresetFromJson(preset, control_root / "data");
            const auto loaded = LoadTerrainPresetFromJson(preset, changed_root / "data");
            const bool uses_fallback = absent && i <= 1;
            EXPECT_EQ(control.ok, uses_fallback);
            EXPECT_EQ(loaded.ok, uses_fallback);
            if (uses_fallback) {
                ASSERT_TRUE(control.params.worldgen_dependencies);
                ASSERT_TRUE(loaded.params.worldgen_dependencies);
                ASSERT_EQ(control.preset_content_hash, loaded.preset_content_hash);
                EXPECT_EQ(control.params.worldgen_dependencies->manifest,
                          loaded.params.worldgen_dependencies->manifest);
            } else {
                EXPECT_FALSE(control.errors.empty());
                EXPECT_FALSE(loaded.errors.empty());
                EXPECT_TRUE(control.preset_content_hash.empty());
                EXPECT_TRUE(loaded.preset_content_hash.empty());
            }
            const auto repaired = changed_root / "data" / dependencies[i];
            fs::remove_all(repaired);
            fs::copy(fs::path(LUMINUMBRA_SOURCE_ROOT) / "data" / dependencies[i],
                     repaired,
                     fs::copy_options::recursive);
            const auto reloaded = LoadTerrainPresetFromJson(preset, changed_root / "data");
            ASSERT_TRUE(reloaded.ok);
            EXPECT_NE(reloaded.preset_content_hash, loaded.preset_content_hash);
            if (uses_fallback) {
                const Luminumbra::Systems::SHIELD_WorldSystem expected(
                    nullptr, nullptr, control.params, 1337);
                const Luminumbra::Systems::SHIELD_WorldSystem actual(
                    nullptr, nullptr, loaded.params, 1337);
                ExpectSameGeneration(actual, expected, {0, 1, 0});
            }
        }
    }
}

TEST(TerrainPresetLoaderTest, UnicodeGenerationOrderMatchesUtf8ManifestOrder) {
    const Luminumbra::TestSupport::ScopedTempFile temp("preset-unicode-order-", ".json");
    const auto data_root = temp.Path().parent_path() / "data";
    const std::vector<fs::path> names = {fs::path(u8"\uE000"), fs::path(u8"\U0001F30D")};
    // UTF-8 sorts E000 before 1F30D; UTF-16 sorts the surrogate pair first.
    for (auto type = names.rbegin(); type != names.rend(); ++type) {
        const auto dir = data_root / "common/structures" / *type;
        fs::create_directories(dir);
        for (auto piece = names.rbegin(); piece != names.rend(); ++piece) {
            auto filename = *piece;
            filename += ".json";
            std::ofstream out(dir / filename, std::ios::binary);
            out << R"({"boxes":[{"size":[1,1,1],"material":)" << (piece == names.rbegin() ? 2 : 1)
                << "}]}";
            out.close();
            ASSERT_TRUE(out.good());
        }
    }
    auto preset = HandoffPreset();
    preset["generation_params"]["biomes"]["table"] = "";
    const auto loaded = LoadTerrainPresetFromJson(preset, data_root);
    ASSERT_TRUE(loaded.ok);
    ASSERT_TRUE(loaded.params.worldgen_dependencies);
    const Luminumbra::Systems::SHIELD_WorldSystem world(nullptr, nullptr, loaded.params, 1337);
    auto direct_params = loaded.params;
    direct_params.worldgen_dependencies.reset();
    const Luminumbra::Systems::SHIELD_WorldSystem direct(nullptr, nullptr, direct_params, 1337);
    ASSERT_EQ(world.structure_pools().size(), names.size());
    ASSERT_EQ(direct.structure_pools().size(), names.size());
    std::vector<std::string> manifest_types;
    for (const auto& record : loaded.params.worldgen_dependencies->manifest) {
        if (record.back() == "pool-accepted")
            manifest_types.push_back(Luminumbra::Filesystem::GenericUtf8PathString(
                Luminumbra::Filesystem::PathFromUtf8(record.front()).filename()));
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto name = Luminumbra::Filesystem::GenericUtf8PathString(names[i]);
        ASSERT_EQ(manifest_types.size(), names.size());
        EXPECT_EQ(manifest_types[i], name);
        EXPECT_EQ(world.structure_pools()[i].type, name);
        EXPECT_EQ(direct.structure_pools()[i].type, name);
        std::vector<std::string> manifest_pieces;
        const std::string prefix = "common/structures/" + name + "/";
        for (const auto& record : loaded.params.worldgen_dependencies->manifest) {
            if (record.front().starts_with(prefix) && record.front() != prefix + "placement.json")
                manifest_pieces.push_back(record.front());
        }
        ASSERT_EQ(manifest_pieces.size(), names.size());
        ASSERT_EQ(world.structure_pools()[i].pieces.size(), names.size());
        ASSERT_EQ(direct.structure_pools()[i].pieces.size(), names.size());
        for (std::size_t j = 0; j < names.size(); ++j) {
            const auto piece = Luminumbra::Filesystem::GenericUtf8PathString(names[j]);
            EXPECT_EQ(manifest_pieces[j], prefix + piece + ".json");
            EXPECT_EQ(world.structure_pools()[i].pieces[j].name, piece);
            EXPECT_EQ(direct.structure_pools()[i].pieces[j].name, piece);
        }
    }
    ExpectSameGeneration(direct, world, {0, 1, 0});
}

TEST(TerrainPresetLoaderTest, DependencyIdentityIgnoresRootLocationAndDirectoryOrder) {
    const Luminumbra::TestSupport::ScopedTempFile temp("preset-dependency-roots-", ".json");
    const auto first_root = temp.Path().parent_path() / fs::path(u8"first-\u732b\U0001f30d");
    const auto second_root = temp.Path().parent_path() / fs::path(u8"second-\u732b\U0001f30d");
    Luminumbra::TestSupport::CopyWorldgenTestData(LUMINUMBRA_SOURCE_ROOT, first_root);
    std::vector<fs::path> files;
    for (const auto& entry : fs::recursive_directory_iterator(first_root / "data")) {
        if (entry.is_regular_file())
            files.push_back(entry.path());
    }
    std::sort(files.rbegin(), files.rend());
    for (const auto& file : files) {
        const auto destination = second_root / file.lexically_relative(first_root);
        fs::create_directories(destination.parent_path());
        fs::copy_file(file, destination);
    }
    auto preset = nlohmann::json::parse(kMinimalPreset);
    preset["schema_rev"] = 7;
    const auto first = LoadTerrainPresetFromJson(preset, first_root / "data", "first");
    const auto second = LoadTerrainPresetFromJson(preset, second_root / "data", "second");
    ASSERT_TRUE(first.ok);
    ASSERT_TRUE(second.ok);
    EXPECT_NE(first.params.biome_table_path, second.params.biome_table_path);
    EXPECT_EQ(first.preset_content_hash, second.preset_content_hash);

    // Logical paths have a fixed UTF-8 encoding, independent of the native path encoding.
    const fs::path unicode_table(u8"common/biomes-\u732b\U0001f30d.json");
    const fs::path unicode_type(u8"\u732b\U0001f30d");
    const fs::path unicode_piece(u8"\u732b\U0001f30d.json");
    for (const auto& root : {first_root, second_root}) {
        fs::copy_file(root / "data/common/biomes.json", root / "data" / unicode_table);
        const auto type = root / "data/common/structures" / unicode_type;
        fs::rename(root / "data/common/structures/cairn", type);
        fs::rename(type / "cairn.json", type / unicode_piece);
    }
    preset["generation_params"]["biomes"]["table"] =
        "common/biomes-\xe7\x8c\xab\xf0\x9f\x8c\x8d.json";
    const auto unicode_first = LoadTerrainPresetFromJson(preset, first_root / "data");
    const auto unicode_second = LoadTerrainPresetFromJson(preset, second_root / "data");
    ASSERT_TRUE(unicode_first.ok);
    ASSERT_TRUE(unicode_second.ok);
    EXPECT_EQ(unicode_first.preset_content_hash, unicode_second.preset_content_hash);
    const auto native_table =
        Luminumbra::Filesystem::PathFromUtf8(unicode_first.params.biome_table_path);
    EXPECT_EQ(native_table, first_root / "data" / unicode_table);
    const auto table = Luminumbra::World::BiomeTable::Load(native_table);
    ASSERT_TRUE(table.ok());
    EXPECT_FALSE(table.empty());
    // Exercise the generator handoff, not just manifest construction.
    const Luminumbra::Systems::SHIELD_WorldSystem world(
        nullptr, nullptr, unicode_first.params, 1337);
    EXPECT_TRUE(world.biomes_enabled());
    EXPECT_EQ(world.biome_table().content_hash(), table.content_hash());
    EXPECT_TRUE(world.structures_enabled());
    const auto type_name = Luminumbra::Filesystem::GenericUtf8PathString(unicode_type);
    EXPECT_TRUE(std::any_of(world.structure_pools().begin(),
                            world.structure_pools().end(),
                            [&](const auto& pool) { return pool.type == type_name; }));
}

TEST(TerrainPresetLoaderTest, AbsentEnabledStructureInputsRefuseWithoutIdentity) {
    const Luminumbra::TestSupport::ScopedTempFile temp("preset-dependency-absent-", ".json");
    const auto root = temp.Path().parent_path();
    Luminumbra::TestSupport::CopyWorldgenTestData(LUMINUMBRA_SOURCE_ROOT, root);
    auto preset = nlohmann::json::parse(kMinimalPreset);
    preset["schema_rev"] = 7;
    const auto full_data = LoadTerrainPresetFromJson(preset, root / "data");
    ASSERT_TRUE(full_data.ok);
    for (const auto& relative :
         {fs::path("common/structures"), fs::path("common/structures/cairn/cairn.json")}) {
        SCOPED_TRACE(Luminumbra::Filesystem::GenericUtf8PathString(relative));
        const auto path = root / "data" / relative;
        const auto hidden = root / "hidden";
        fs::rename(path, hidden);
        const auto absent = LoadTerrainPresetFromJson(preset, root / "data");
        EXPECT_FALSE(absent.ok);
        EXPECT_FALSE(absent.errors.empty());
        EXPECT_TRUE(absent.preset_content_hash.empty());
        fs::rename(hidden, path);
        const auto restored = LoadTerrainPresetFromJson(preset, root / "data");
        ASSERT_TRUE(restored.ok);
        EXPECT_EQ(restored.preset_content_hash, full_data.preset_content_hash);
    }
}

TEST(TerrainPresetLoaderTest, PresentUnreadableWorldgenInputsRefuseWithoutIdentity) {
    const Luminumbra::TestSupport::ScopedTempFile temp("preset-dependency-unreadable-", ".json");
    const auto root = temp.Path().parent_path();
    Luminumbra::TestSupport::CopyWorldgenTestData(LUMINUMBRA_SOURCE_ROOT, root);
    auto preset = nlohmann::json::parse(kMinimalPreset);
    preset["schema_rev"] = 7;
    // A directory at a file path is reliably unreadable on every platform,
    // including privileged runners where removing permission bits has no effect.
    for (const auto& relative :
         {fs::path("common/biomes.json"), fs::path("common/structures/cairn/placement.json")}) {
        const auto path = root / "data" / relative;
        const auto hidden = root / "hidden";
        fs::rename(path, hidden);
        fs::create_directory(path);
        const auto unreadable = LoadTerrainPresetFromJson(preset, root / "data");
        EXPECT_FALSE(unreadable.ok);
        EXPECT_TRUE(unreadable.preset_content_hash.empty());
        ASSERT_FALSE(unreadable.errors.empty());
        EXPECT_NE(unreadable.errors.front().find("worldgen"), std::string::npos);
        fs::remove(path);
        fs::rename(hidden, path);
        ASSERT_TRUE(LoadTerrainPresetFromJson(preset, root / "data").ok);
    }
}

TEST(TerrainPresetLoaderTest, BiomeStatusErrorFallsBackDirectlyButRefusesPreset) {
    const Luminumbra::TestSupport::ScopedTempFile temp("preset-biome-status-", ".json");
    const auto root = temp.Path().parent_path();
    // An over-long component is unavailable even on privileged runners, but
    // Windows runtimes can classify it as not_found with a nonzero error_code.
    const auto relative =
        Luminumbra::Filesystem::PathFromUtf8(std::string(4096, 'x')) / "biomes.json";
    const auto path = root / relative;
    std::error_code ec;
    EXPECT_FALSE(fs::is_regular_file(path, ec));
    ASSERT_TRUE(ec);
    const auto resolved_path =
        root / Luminumbra::Filesystem::LexicallyNormalPath(Luminumbra::Filesystem::PathFromUtf8(
                   Luminumbra::Filesystem::GenericUtf8PathString(relative)));
    ASSERT_EQ(resolved_path, path);
    const auto native_status = fs::symlink_status(resolved_path, ec);
    RecordProperty("native_biome_status_type", static_cast<int>(native_status.type()));
    RecordProperty("native_biome_status_error", ec.value());
    RecordProperty("native_biome_status_category", ec.category().name());
    std::string bytes = "previous contents";
    EXPECT_FALSE(Luminumbra::World::ReadWorldgenFile(path, bytes));
    EXPECT_TRUE(bytes.empty());
    Luminumbra::World::BiomeTable unavailable;
    ASSERT_NO_THROW(unavailable = Luminumbra::World::BiomeTable::Load(path));
    EXPECT_FALSE(unavailable.ok());
    EXPECT_TRUE(unavailable.empty());
    EXPECT_FALSE(unavailable.errors().empty());

    // Inject an I/O status failure through the same query used by the
    // reader and manifest, rather than depending on native not-found mappings.
    const Luminumbra::World::WorldgenFileStatusReader read_status = [&](const fs::path& queried,
                                                                        std::error_code& error) {
        EXPECT_EQ(queried, path);
        error = std::make_error_code(std::errc::io_error);
        return fs::file_status(fs::file_type::none);
    };
    ASSERT_EQ(read_status(path, ec).type(), fs::file_type::none);
    ASSERT_EQ(ec, std::errc::io_error);
    const Luminumbra::World::WorldgenFileReader read_file = [&](const fs::path& file,
                                                                std::string& contents) {
        return Luminumbra::World::ReadWorldgenFileWithStatus(file, contents, read_status);
    };
    bytes = "previous contents";
    EXPECT_FALSE(read_file(path, bytes));
    EXPECT_TRUE(bytes.empty());
    Luminumbra::World::BiomeTable table;
    ASSERT_NO_THROW(table = Luminumbra::World::BiomeTable::Load(path, read_file));
    EXPECT_FALSE(table.ok());
    EXPECT_TRUE(table.empty());
    EXPECT_FALSE(table.errors().empty());

    Luminumbra::Systems::TerrainGenParams params;
    params.biomes_enabled = true;
    params.biome_table_path = Luminumbra::Filesystem::GenericUtf8PathString(path);
    params.structures_enabled = false;
    ASSERT_FALSE(params.worldgen_dependencies);
    ASSERT_NO_THROW({
        const Luminumbra::Systems::SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
        EXPECT_FALSE(world.biomes_enabled());
        EXPECT_FALSE(world.biome_table().ok());
        const auto biome = world.BiomeIdAt(0.0f, 0.0f);
        EXPECT_EQ(biome, Luminumbra::World::kNoBiome);
        using Luminumbra::MaterialType;
        EXPECT_EQ(world.SurfaceMaterialForColumn(32.0f, 33.0f, biome), MaterialType::Sand);
        EXPECT_EQ(world.SurfaceMaterialForColumn(39.5f, 40.0f, biome), MaterialType::Grass);
        EXPECT_EQ(world.SurfaceMaterialForColumn(38.0f, 40.0f, biome), MaterialType::Soil);
        EXPECT_EQ(world.SurfaceMaterialForColumn(34.0f, 40.0f, biome), MaterialType::Stone);
    });

    auto preset = HandoffPreset();
    preset["generation_params"]["biomes"]["table"] =
        Luminumbra::Filesystem::GenericUtf8PathString(relative);
    preset["generation_params"]["features"]["structures_enabled"] = false;
    TerrainPresetLoadResult refused;
    ASSERT_NO_THROW(refused = LoadTerrainPresetFromJson(preset, root, "<memory>", read_status));
    EXPECT_FALSE(refused.ok);
    EXPECT_TRUE(refused.preset_content_hash.empty());
    EXPECT_FALSE(refused.params.worldgen_dependencies);
    ASSERT_FALSE(refused.errors.empty());
    EXPECT_NE(refused.errors.front().find("worldgen"), std::string::npos);
}

TEST(TerrainPresetLoaderTest, DanglingPlacementUsesAbsentDefaultsAndIdentity) {
    const Luminumbra::TestSupport::ScopedTempFile temp("preset-placement-link-", ".json");
    const auto root = temp.Path().parent_path();
    Luminumbra::TestSupport::CopyWorldgenTestData(LUMINUMBRA_SOURCE_ROOT, root);
    const auto preset = HandoffPreset();
    const auto type = root / "data/common/structures/cairn";
    const auto placement = type / "placement.json";
    const auto authored = LoadTerrainPresetFromJson(preset, root / "data");
    ASSERT_TRUE(authored.ok);
    const auto authored_pool = Luminumbra::World::LoadStructureTemplatePool(type, "cairn");
    ASSERT_TRUE(authored_pool.ok());
    EXPECT_EQ(authored_pool.spacing, 96);
    EXPECT_EQ(authored_pool.salt, 101u);

    ASSERT_TRUE(fs::remove(placement));
    const auto absent_pool = Luminumbra::World::LoadStructureTemplatePool(type, "cairn");
    ASSERT_TRUE(absent_pool.ok());
    EXPECT_FALSE(absent_pool.io_failed);
    EXPECT_EQ(absent_pool.spacing, 64);
    EXPECT_EQ(absent_pool.separation, 16);
    EXPECT_EQ(absent_pool.salt, 0u);
    EXPECT_FLOAT_EQ(absent_pool.density, 1.0f);
    const auto absent = LoadTerrainPresetFromJson(preset, root / "data");
    ASSERT_TRUE(absent.ok);
    ASSERT_TRUE(absent.params.worldgen_dependencies);
    EXPECT_NE(absent.preset_content_hash, authored.preset_content_hash);
    const auto& manifest = absent.params.worldgen_dependencies->manifest;
    EXPECT_NE(std::find(manifest.begin(),
                        manifest.end(),
                        std::vector<std::string>{"common/structures/cairn/placement.json",
                                                 "placement-absent:compiled-defaults"}),
              manifest.end());
    const Luminumbra::Systems::SHIELD_WorldSystem expected(nullptr, nullptr, absent.params, 1337);
    ASSERT_TRUE(expected.structures_enabled());

    std::error_code ec;
    fs::create_symlink("missing-placement.json", placement, ec);
#ifdef _WIN32
    // Only symlink coverage needs Windows developer mode/privilege and a
    // runtime that supports symlinks; absent-defaults coverage always runs.
    if (ec.value() != 1314 && ec != std::errc::function_not_supported &&
        ec != std::errc::operation_not_supported)
#endif
    {
        ASSERT_FALSE(ec) << ec.message();
        ASSERT_TRUE(fs::is_symlink(fs::symlink_status(placement)));
        ASSERT_FALSE(fs::exists(placement));
        const auto linked_pool = Luminumbra::World::LoadStructureTemplatePool(type, "cairn");
        ASSERT_TRUE(linked_pool.ok());
        EXPECT_FALSE(linked_pool.io_failed);
        EXPECT_EQ(linked_pool.content_hash, absent_pool.content_hash);
        const auto linked = LoadTerrainPresetFromJson(preset, root / "data");
        ASSERT_TRUE(linked.ok);
        EXPECT_EQ(linked.preset_content_hash, absent.preset_content_hash);
        ASSERT_TRUE(linked.params.worldgen_dependencies);
        EXPECT_EQ(linked.params.worldgen_dependencies->manifest, manifest);
        const Luminumbra::Systems::SHIELD_WorldSystem snapshot(
            nullptr, nullptr, linked.params, 1337);
        ExpectSameGeneration(snapshot, expected, Luminumbra::IVec3(0, 1, 0));
        auto direct_params = linked.params;
        direct_params.worldgen_dependencies.reset();
        const Luminumbra::Systems::SHIELD_WorldSystem direct(nullptr, nullptr, direct_params, 1337);
        ExpectSameGeneration(direct, expected, Luminumbra::IVec3(0, 1, 0));
    }
}

void ExpectDependencyRefusal(const TerrainPresetLoadResult& result) {
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.errors.empty());
    EXPECT_TRUE(result.preset_content_hash.empty());
    EXPECT_FALSE(result.params.worldgen_dependencies);
}

TEST(TerrainPresetLoaderTest, RejectedBiomeTableRefusesWithoutIdentity) {
    const Luminumbra::TestSupport::ScopedTempFile temp("preset-biome-rejected-", ".json");
    const auto root = temp.Path().parent_path();
    Luminumbra::TestSupport::CopyWorldgenTestData(LUMINUMBRA_SOURCE_ROOT, root);
    auto preset = nlohmann::json::parse(kMinimalPreset);
    preset["schema_rev"] = 7;
    const auto path = root / "data/common/biomes.json";
    for (const char* bytes : {"{}", "{ }", "{invalid", R"({"biomes":[]})"}) {
        SCOPED_TRACE(bytes);
        std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
        const auto rejected = LoadTerrainPresetFromJson(preset, root / "data");
        ExpectDependencyRefusal(rejected);
    }
}

TEST(TerrainPresetLoaderTest, AbsentBiomeTableRetainsLegacyClassificationSnapshotAndIdentity) {
    const Luminumbra::TestSupport::ScopedTempFile temp("preset-biome-absent-", ".json");
    const auto root = temp.Path().parent_path();
    auto preset = HandoffPreset();
    preset["generation_params"]["features"]["structures_enabled"] = false;
    const auto absent = LoadTerrainPresetFromJson(preset, root / "data");
    ASSERT_TRUE(absent.ok);
    EXPECT_TRUE(absent.errors.empty());
    EXPECT_FALSE(absent.preset_content_hash.empty());
    ASSERT_TRUE(absent.params.worldgen_dependencies);
    const std::vector<std::vector<std::string>> expected_manifest = {
        {"common/biomes.json", "biome-table-absent:legacy-classification"}};
    EXPECT_EQ(absent.params.worldgen_dependencies->manifest, expected_manifest);
    std::ofstream(temp.Path(), std::ios::binary | std::ios::trunc) << preset.dump();
    const auto from_disk = LoadTerrainPreset(temp.Path(), root / "data");
    ASSERT_TRUE(from_disk.ok);
    EXPECT_EQ(absent.preset_content_hash, from_disk.preset_content_hash);
    const auto elsewhere = LoadTerrainPresetFromJson(preset, root / "other/data");
    ASSERT_TRUE(elsewhere.ok);
    EXPECT_EQ(absent.preset_content_hash, elsewhere.preset_content_hash);
    const Luminumbra::Systems::SHIELD_WorldSystem expected(nullptr, nullptr, absent.params, 1337);
    EXPECT_FALSE(expected.biomes_enabled());
    EXPECT_EQ(expected.BiomeIdAt(0.0f, 0.0f), Luminumbra::World::kNoBiome);
    auto legacy_params = absent.params;
    legacy_params.worldgen_dependencies.reset();
    legacy_params.biomes_enabled = false;
    const Luminumbra::Systems::SHIELD_WorldSystem legacy(nullptr, nullptr, legacy_params, 1337);
    ExpectSameGeneration(expected, legacy, {0, 1, 0});

    Luminumbra::TestSupport::CopyWorldgenTestData(LUMINUMBRA_SOURCE_ROOT, root);
    const auto present = LoadTerrainPresetFromJson(preset, root / "data");
    ASSERT_TRUE(present.ok);
    EXPECT_NE(absent.preset_content_hash, present.preset_content_hash);
    // Adopting the absent snapshot must not load a table created during handoff.
    const Luminumbra::Systems::SHIELD_WorldSystem actual(nullptr, nullptr, absent.params, 1337);
    ExpectSameGeneration(actual, expected, {0, 1, 0});
}

TEST(TerrainPresetLoaderTest, RejectedOrEmptyPoolsRefuseWithoutIdentity) {
    const Luminumbra::TestSupport::ScopedTempFile temp("preset-dependency-rejected-", ".json");
    const auto root = temp.Path().parent_path();
    Luminumbra::TestSupport::CopyWorldgenTestData(LUMINUMBRA_SOURCE_ROOT, root);
    auto preset = nlohmann::json::parse(kMinimalPreset);
    preset["schema_rev"] = 7;
    const auto type = root / "data/common/structures/cairn";
    const auto baseline = LoadTerrainPresetFromJson(preset, root / "data");
    ASSERT_TRUE(baseline.ok);
    // Malformed placement rejects the pool before it reads any pieces.
    std::ofstream(type / "placement.json", std::ios::binary) << "{";
    EXPECT_FALSE(Luminumbra::World::LoadStructureTemplatePool(type, "cairn").ok());
    const auto rejected = LoadTerrainPresetFromJson(preset, root / "data");
    ExpectDependencyRefusal(rejected);
    std::ofstream(type / "cairn.json", std::ios::binary) << "{bad piece";
    const auto unread_piece = LoadTerrainPresetFromJson(preset, root / "data");
    ExpectDependencyRefusal(unread_piece);
    std::ofstream(type / "placement.json", std::ios::binary) << "{ ";
    const auto changed_rejection = LoadTerrainPresetFromJson(preset, root / "data");
    ExpectDependencyRefusal(changed_rejection);

    // With optional placement absent, the generator reads and rejects the piece.
    fs::remove(type / "placement.json");
    const auto rejected_piece = LoadTerrainPresetFromJson(preset, root / "data");
    ExpectDependencyRefusal(rejected_piece);
    EXPECT_FALSE(Luminumbra::World::LoadStructureTemplatePool(type, "cairn").ok());
    std::ofstream(type / "cairn.json", std::ios::binary) << "{different bad piece";
    const auto different_piece = LoadTerrainPresetFromJson(preset, root / "data");
    ExpectDependencyRefusal(different_piece);
    std::ofstream(type / "cairn.json", std::ios::binary) << "{}"; // valid JSON, empty piece
    const auto empty_piece = LoadTerrainPresetFromJson(preset, root / "data");
    ExpectDependencyRefusal(empty_piece);
    EXPECT_FALSE(Luminumbra::World::LoadStructureTemplatePool(type, "cairn").ok());
}

TEST(TerrainPresetLoaderTest, OptionalPlacementAndUnresolvedFilesFollowGeneratorSelection) {
    const Luminumbra::TestSupport::ScopedTempFile temp("preset-dependency-selection-", ".json");
    const auto root = temp.Path().parent_path();
    Luminumbra::TestSupport::CopyWorldgenTestData(LUMINUMBRA_SOURCE_ROOT, root);
    auto preset = nlohmann::json::parse(kMinimalPreset);
    preset["schema_rev"] = 7;
    const auto first = LoadTerrainPresetFromJson(preset, root / "data");
    ASSERT_TRUE(first.ok);
    const auto type = root / "data/common/structures/cairn";
    std::ofstream(type / "ignored.txt") << "not a template";
    fs::create_directory(type / "nested");
    std::ofstream(type / "nested/ignored.json") << "{}";
    const auto ignored = LoadTerrainPresetFromJson(preset, root / "data");
    ASSERT_TRUE(ignored.ok);
    EXPECT_EQ(ignored.preset_content_hash, first.preset_content_hash);
    ASSERT_TRUE(fs::remove(type / "placement.json"));
    const auto optional = LoadTerrainPresetFromJson(preset, root / "data");
    ASSERT_TRUE(optional.ok); // the generator uses compiled placement defaults when omitted
    EXPECT_NE(optional.preset_content_hash, first.preset_content_hash);
    preset["generation_params"]["biomes"]["table"] = "";
    preset["generation_params"]["features"]["structures_enabled"] = false;
    const auto disabled = LoadTerrainPresetFromJson(preset, root / "absent-data");
    ASSERT_TRUE(disabled.ok);
    EXPECT_EQ(disabled.preset_content_hash, Luminumbra::world::TerrainPresetContentHash(preset));
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
    ASSERT_TRUE(Luminumbra::Filesystem::IsAbsolutePath(
        Luminumbra::Filesystem::PathFromUtf8(result.params.biome_table_path)));
    std::ifstream input(Luminumbra::Filesystem::PathFromUtf8(result.params.biome_table_path),
                        std::ios::binary);
    ASSERT_TRUE(input.is_open()) << result.params.biome_table_path;
    EXPECT_FALSE(Luminumbra::World::BiomeTable::Load(
                     Luminumbra::Filesystem::PathFromUtf8(result.params.biome_table_path))
                     .empty());
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
        EXPECT_EQ(nlohmann::json::parse(input).at("schema_rev"), 8) << name;
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
    for (const bool explicit_revision : {false, true}) {
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
        const auto result =
            LoadTerrainPresetFromJson(preset, PresetDir() / "../../../data", "content-switches");
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
    for (const int revision : {6, 7}) {
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
        const auto reopened = LoadTerrainPresetFromJson(saved, PresetDir() / "../../../data");
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
    for (const int revision : {6, 7}) {
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
    for (const int revision : {6, 7}) {
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
    for (const int revision : {6, 7}) {
        for (const bool empty_blocks : {false, true}) {
            SCOPED_TRACE(revision);
            SCOPED_TRACE(empty_blocks);
            auto preset = nlohmann::json::parse(kMinimalPreset);
            preset["schema_rev"] = revision;
            if (empty_blocks) {
                preset["generation_params"]["terrain"]["hydro"] = nlohmann::json::object();
                preset["generation_params"]["biomes"] = nlohmann::json::object();
            }
            const auto result = LoadTerrainPresetFromJson(preset, PresetDir() / "../../../data");
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
