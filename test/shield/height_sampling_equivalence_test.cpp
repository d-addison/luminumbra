#include "../fixtures/TerrainFixtureParams.h"
#include "luminumbra/core/Types.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/FarLodStore.h"
#include "luminumbra_common/world/HydraulicErosion.h"
#include "luminumbra_common/world/TerrainPresetLoader.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Luminumbra::Chunk;
using Luminumbra::IVec3;
using Luminumbra::Vec3;
using Luminumbra::Systems::SHIELD_WorldSystem;

constexpr std::array<const char*, 8> kShippingPresets{"amplified",
                                                      "archipelago",
                                                      "cave_dense",
                                                      "caverns",
                                                      "default",
                                                      "flat_lands",
                                                      "mountains",
                                                      "temperate_forest"};

std::filesystem::path PresetPath(const char* name) {
    return std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets" /
           (std::string(name) + ".json");
}

std::uint32_t Bits(float value) {
    return std::bit_cast<std::uint32_t>(value);
}

class HeightSamplingEquivalence : public ::testing::TestWithParam<const char*> {};

TEST(HeightSamplingCatalog, CoversEveryShippingPreset) {
    std::vector<std::string> actual;
    for (const auto& entry :
         std::filesystem::directory_iterator(PresetPath("default").parent_path())) {
        if (entry.path().extension() == ".json") {
            actual.push_back(entry.path().stem().string());
        }
    }
    std::sort(actual.begin(), actual.end());
    const std::vector<std::string> expected(kShippingPresets.begin(), kShippingPresets.end());
    EXPECT_EQ(actual, expected);
}

TEST_P(HeightSamplingEquivalence, MatchesGeneratedGridsAndScalarBitsAcrossBoundaries) {
    const auto loaded = Luminumbra::world::LoadTerrainPreset(PresetPath(GetParam()));
    ASSERT_TRUE(loaded.ok);
    // Footprints cross chunk, doline (110/128 m), hydro (512 m), and lake (768 m)
    // boundaries, including negative floor-division and shared corner samples.
    const std::array<IVec3, 7> chunks{IVec3(0, 0, 0),
                                      IVec3(31, 0, 31),
                                      IVec3(-33, 0, -33),
                                      IVec3(47, 0, -49),
                                      IVec3(7, 0, -9),
                                      IVec3(6, 0, -7),
                                      IVec3(-7, 0, 6)};
    for (const int seed : {1337, 8675309}) {
        for (const bool hydro : {false, loaded.params.hydro_enabled}) {
            SCOPED_TRACE(::testing::Message()
                         << GetParam() << " seed=" << seed << " hydro=" << hydro);
            auto params = loaded.params;
            params.surface_sampling_revision = Luminumbra::Systems::kSurfaceSamplingRevision;
            params.hydro_enabled = hydro;
            const SHIELD_WorldSystem world(nullptr, nullptr, params, seed);
            for (const auto& coords : chunks) {
                const auto base = coords * IVec3(Luminumbra::CHUNK_SIZE_X,
                                                 Luminumbra::CHUNK_SIZE_Y,
                                                 Luminumbra::CHUNK_SIZE_Z);
                std::array<float,
                           static_cast<std::size_t>(Luminumbra::CHUNK_SIZE_X + 1) *
                               static_cast<std::size_t>(Luminumbra::CHUNK_SIZE_Z + 1)>
                    xs{};
                auto zs = xs;
                auto heights = xs;
                std::size_t index = 0;
                for (int z = 0; z <= Luminumbra::CHUNK_SIZE_Z; ++z) {
                    for (int x = 0; x <= Luminumbra::CHUNK_SIZE_X; ++x) {
                        xs[index] = static_cast<float>(base.x + x);
                        zs[index] = static_cast<float>(base.z + z);
                        ++index;
                    }
                }
                world.ComputeShapedHeightsAtPositions(
                    xs.data(), zs.data(), xs.size(), heights.data());
                for (const int step : {1, 2}) {
                    Chunk chunk(coords);
                    world.GenerateChunkData(chunk, step);
                    ASSERT_EQ(chunk.heightmap_data.size(), heights.size());
                    for (std::size_t i = 0; i < heights.size(); ++i) {
                        SCOPED_TRACE(::testing::Message()
                                     << "x=" << xs[i] << " z=" << zs[i] << " step=" << step);
                        EXPECT_EQ(Bits(heights[i]), Bits(chunk.heightmap_data[i]));
                        EXPECT_EQ(Bits(heights[i]), Bits(world.GetTerrainHeightAt(xs[i], zs[i])));
                    }
                }
            }

            // Fractional samples on both sides of hydro seams; tiny and partial
            // SIMD batches also prove that callers need no padding of their own.
            const std::array<float, 17> xs{511.75f,
                                           512.0f,
                                           512.25f,
                                           -512.25f,
                                           -512.0f,
                                           -511.75f,
                                           127.75f,
                                           128.0f,
                                           128.25f,
                                           -128.25f,
                                           -128.0f,
                                           -127.75f,
                                           767.75f,
                                           768.0f,
                                           768.25f,
                                           -768.25f,
                                           -767.75f};
            auto zs = xs;
            std::reverse(zs.begin(), zs.end());
            auto in_place_xs = xs;
            world.ComputeShapedHeightsAtPositions(
                in_place_xs.data(), zs.data(), xs.size(), in_place_xs.data());
            for (std::size_t i = 0; i < xs.size(); ++i) {
                EXPECT_EQ(Bits(in_place_xs[i]), Bits(world.GetTerrainHeightAt(xs[i], zs[i])));
            }
            for (const std::size_t count : {0u, 1u, 5u, 15u, 16u, 17u}) {
                std::array<float, 19> heights{};
                heights.fill(123456.0f);
                world.ComputeShapedHeightsAtPositions(
                    xs.data(), zs.data(), count, heights.data() + 1);
                EXPECT_EQ(heights.front(), 123456.0f);
                EXPECT_EQ(heights[count + 1], 123456.0f);
                for (std::size_t i = 0; i < count; ++i) {
                    EXPECT_EQ(Bits(heights[i + 1]), Bits(world.GetTerrainHeightAt(xs[i], zs[i])));
                }
            }
            if (hydro) {
                auto analytic_params = params;
                analytic_params.hydro_enabled = false;
                const SHIELD_WorldSystem analytic(nullptr, nullptr, analytic_params, seed);
                std::array<float, 17> heights{};
                world.ComputeShapedHeightsAtPositions(
                    xs.data(), zs.data(), xs.size(), heights.data(), false);
                for (std::size_t i = 0; i < heights.size(); ++i) {
                    EXPECT_EQ(Bits(heights[i]), Bits(analytic.GetTerrainHeightAt(xs[i], zs[i])));
                }
            }
        }
    }
}

INSTANTIATE_TEST_SUITE_P(ShippingPresets,
                         HeightSamplingEquivalence,
                         ::testing::ValuesIn(kShippingPresets));

TEST(HeightSamplingEquivalenceStages, EachPreviouslyMissingStageChangesSampledHeights) {
    const auto loaded = Luminumbra::world::LoadTerrainPreset(PresetPath("default"));
    ASSERT_TRUE(loaded.ok);
    for (int stage = 0; stage < 4; ++stage) {
        SCOPED_TRACE(stage);
        auto params = loaded.params;
        params.base_amplitude = 0.0f;
        params.height_offset = 47.0f;
        params.continental_spline = {{-1.0f, 0.0f}, {1.0f, 0.0f}};
        params.peaks_spline = {{-1.0f, 1.0f}, {1.0f, 1.0f}};
        params.peaks_amplitude = stage == 0 ? 20.0f : 0.0f;
        params.biome_relief_enabled = stage == 0;
        params.cliffs_enabled = stage == 1;
        params.cliff_threshold = -2.0f;
        params.cliff_step = 48.0f;
        params.lakes_enabled = stage == 2;
        params.lake_threshold = -2.0f;
        params.surface_breaks_enabled = stage == 3;
        params.surface_break_density = 1.0f;
        params.island_mask_enabled = false;
        params.rivers_enabled = false;
        params.hydro_enabled = false;
        params.structures_enabled = false;
        const SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
        params.biome_relief_enabled = false;
        params.cliffs_enabled = false;
        params.lakes_enabled = false;
        params.surface_breaks_enabled = false;
        const SHIELD_WorldSystem reference(nullptr, nullptr, params, 1337);
        std::array<float, 5> xs{16.0f, -128.0f, 512.0f, -768.0f, 97.25f};
        std::array<float, 5> zs{128.0f, -16.0f, -512.0f, 768.0f, -31.75f};
        if (stage == 3) {
            const auto feature = world.FindLargestSurfaceBreak(0.0f, 0.0f, 512.0f);
            ASSERT_TRUE(feature.found);
            xs.back() = feature.x;
            zs.back() = feature.z;
        }
        std::array<float, 5> heights{};
        world.ComputeShapedHeightsAtPositions(xs.data(), zs.data(), xs.size(), heights.data());
        bool changed = false;
        for (std::size_t i = 0; i < heights.size(); ++i) {
            EXPECT_EQ(Bits(heights[i]), Bits(world.GetTerrainHeightAt(xs[i], zs[i])));
            changed =
                changed || Bits(heights[i]) != Bits(reference.GetTerrainHeightAt(xs[i], zs[i]));
        }
        EXPECT_TRUE(changed) << "stage must affect the sampled positions";
    }
}

TEST(HeightSamplingCompatibility, ErosionRetainsHistoricalSubstrate) {
    const auto loaded = Luminumbra::world::LoadTerrainPreset(PresetPath("caverns"));
    ASSERT_TRUE(loaded.ok);
    auto analytic_params = loaded.params;
    analytic_params.hydro_enabled = false;
    auto substrate_params = analytic_params;
    substrate_params.biome_relief_enabled = false;
    substrate_params.cliffs_enabled = false;
    substrate_params.lakes_enabled = false;
    substrate_params.surface_breaks_enabled = false;
    constexpr int seed = 1337;
    const SHIELD_WorldSystem substrate(nullptr, nullptr, substrate_params, seed);
    const SHIELD_WorldSystem analytic(nullptr, nullptr, analytic_params, seed);

    // Rebuild the historical region (0,0) independently through public scalar
    // sampling, then the erosion kernel. This pins the compatibility boundary,
    // rather than comparing two consumers of the same cached offset.
    constexpr int region_cells = 64;
    const int halo = loaded.params.hydro_iterations + 8;
    const int side = region_cells + 2 * halo;
    const float cell_size = loaded.params.hydro_cell_size_m;
    std::vector<float> heights(static_cast<std::size_t>(side) * static_cast<std::size_t>(side));
    for (int z = 0; z < side; ++z) {
        for (int x = 0; x < side; ++x) {
            heights[static_cast<std::size_t>(z) * static_cast<std::size_t>(side) +
                    static_cast<std::size_t>(x)] =
                substrate.GetTerrainHeightAt(static_cast<float>(x - halo) * cell_size,
                                             static_cast<float>(z - halo) * cell_size);
        }
    }
    const auto& params = loaded.params;
    const Luminumbra::World::HydroErosionParams erosion{
        .iterations = params.hydro_iterations,
        .talus_height = params.hydro_talus_height,
        .thermal_rate = params.hydro_thermal_rate,
        .rain_per_sweep = params.hydro_rain_per_sweep,
        .solubility = params.hydro_solubility,
        .deposition = params.hydro_deposition,
        .evaporation = params.hydro_evaporation,
        .sediment_capacity = params.hydro_sediment_capacity,
        .max_offset = params.hydro_max_offset};
    std::vector<float> offsets;
    Luminumbra::World::BakeHydraulicErosion(heights, region_cells, halo, erosion, offsets);
    ASSERT_EQ(offsets.size(), static_cast<std::size_t>(region_cells * region_cells));
    bool nonzero = false;
    for (const int revision : {1, 2}) {
        auto world_params = params;
        world_params.surface_sampling_revision = revision;
        const SHIELD_WorldSystem world(nullptr, nullptr, world_params, seed);
        for (int z = 1; z < region_cells - 1; z += 7) {
            for (int x = 1; x < region_cells - 1; x += 7) {
                const float offset =
                    offsets[static_cast<std::size_t>(z) * static_cast<std::size_t>(region_cells) +
                            static_cast<std::size_t>(x)];
                nonzero = nonzero || offset != 0.0f;
                const float wx = static_cast<float>(x) * cell_size;
                const float wz = static_cast<float>(z) * cell_size;
                EXPECT_EQ(Bits(world.GetTerrainHeightAt(wx, wz)),
                          Bits(analytic.GetTerrainHeightAt(wx, wz) + offset));
            }
        }
    }
    EXPECT_TRUE(nonzero);
}

TEST(HeightSamplingReset, ParametersAndSeedsMatchFreshInstances) {
    const auto loaded = Luminumbra::world::LoadTerrainPreset(PresetPath("default"));
    ASSERT_TRUE(loaded.ok);
    for (const int revision : {1, 2}) {
        for (const bool change_seed : {false, true}) {
            SCOPED_TRACE(::testing::Message() << revision << " seed=" << change_seed);
            auto params = loaded.params;
            params.surface_sampling_revision = revision;
            params.hydro_enabled = true;
            SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
            auto analytic_params = params;
            analytic_params.hydro_enabled = false;
            const SHIELD_WorldSystem analytic(nullptr, nullptr, analytic_params, 1337);
            std::array<float, 64> original{};
            bool erosion_observed = false;
            for (std::size_t i = 0; i < original.size(); ++i) {
                const float x = static_cast<float>(1 + (i % 8) * 7) * params.hydro_cell_size_m;
                const float z = static_cast<float>(1 + (i / 8) * 7) * params.hydro_cell_size_m;
                original[i] = world.GetTerrainHeightAt(x, z);
                erosion_observed = erosion_observed ||
                                   Bits(original[i]) != Bits(analytic.GetTerrainHeightAt(x, z));
            }
            ASSERT_TRUE(erosion_observed);
            auto next_params = params;
            const int next_seed = change_seed ? 8675309 : 1337;
            if (change_seed)
                world.set_seed(next_seed);
            else {
                next_params.hydro_max_offset = 0.0f;
                world.set_params(next_params);
            }
            const SHIELD_WorldSystem fresh(nullptr, nullptr, next_params, next_seed);
            bool changed = false;
            for (std::size_t i = 0; i < original.size(); ++i) {
                const float x = static_cast<float>(1 + (i % 8) * 7) * params.hydro_cell_size_m;
                const float z = static_cast<float>(1 + (i / 8) * 7) * params.hydro_cell_size_m;
                const float actual = world.GetTerrainHeightAt(x, z);
                EXPECT_EQ(Bits(actual), Bits(fresh.GetTerrainHeightAt(x, z)));
                changed = changed || Bits(actual) != Bits(original[i]);
            }
            EXPECT_TRUE(changed);
            // Switching back also invalidates the new generation's offsets.
            world.set_params(params);
            world.set_seed(1337);
            for (std::size_t i = 0; i < original.size(); ++i) {
                const float x = static_cast<float>(1 + (i % 8) * 7) * params.hydro_cell_size_m;
                const float z = static_cast<float>(1 + (i / 8) * 7) * params.hydro_cell_size_m;
                EXPECT_EQ(Bits(world.GetTerrainHeightAt(x, z)), Bits(original[i]));
            }
        }
    }
}

TEST(HeightSamplingReset, ReaderRetainsOldGenerationUntilItsEpochEnds) {
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.hydro_enabled = true;
    params.hydro_iterations = 1;
    SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    const float old_height = world.GetTerrainHeightAt(8.0f, 8.0f);
    auto next_params = params;
    next_params.hydro_max_offset = 0.0f;
    next_params.height_offset += 64.0f;
    std::promise<void> resetting;
    auto started = resetting.get_future();
    std::future<void> reset;
    {
        const auto scope = world.acquire_worldgen_sample_scope();
        reset = std::async(std::launch::async, [&]() {
            resetting.set_value();
            world.set_params(next_params);
        });
        started.get();
        // The writer cannot clear or replace the generation while this reader
        // owns its epoch, including immutable regions retained by chunk sampling.
        for (int i = 0; i < 4; ++i)
            EXPECT_EQ(Bits(world.GetTerrainHeightAt(8.0f, 8.0f)), Bits(old_height));
    }
    reset.get();
    const SHIELD_WorldSystem fresh(nullptr, nullptr, next_params, 1337);
    EXPECT_EQ(Bits(world.GetTerrainHeightAt(8.0f, 8.0f)),
              Bits(fresh.GetTerrainHeightAt(8.0f, 8.0f)));
    EXPECT_NE(Bits(world.GetTerrainHeightAt(8.0f, 8.0f)), Bits(old_height));
}

TEST(HeightSamplingReset, QueuedPrefetchDrainsBeforeResetAndWorldDestruction) {
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.hydro_enabled = true;
    params.hydro_iterations = 1;
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    {
        SHIELD_WorldSystem world(&jobs, nullptr, params, 1337);
        std::promise<void> release_worker;
        const auto released = release_worker.get_future().share();
        jobs.dispatch([released]() { released.wait(); });
        // Radius zero queues exactly nine regions, all behind the blocked worker.
        world.PrefetchHydroRegions(8.0f, 8.0f, 0.0f);
        auto reset = std::async(std::launch::async, [&]() { world.set_seed(8675309); });
        release_worker.set_value();
        reset.get();
        const SHIELD_WorldSystem fresh(nullptr, nullptr, params, 8675309);
        EXPECT_EQ(Bits(world.GetTerrainHeightAt(8.0f, 8.0f)),
                  Bits(fresh.GetTerrainHeightAt(8.0f, 8.0f)));
        world.PrefetchHydroRegions(-512.0f, -512.0f, 0.0f);
        // Destruction must settle these jobs before any member storage goes away.
    }
    jobs.shutdown();
}

TEST(HeightSamplingReset, RejectedPrefetchReleasesItsOutstandingOwnership) {
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.hydro_enabled = true;
    params.hydro_iterations = 1;
    Luminumbra::JobSystem stopped_jobs;
    SHIELD_WorldSystem world(&stopped_jobs, nullptr, params, 1337);
    world.PrefetchHydroRegions(8.0f, 8.0f, 0.0f);
    world.set_seed(8675309);
    const SHIELD_WorldSystem fresh(nullptr, nullptr, params, 8675309);
    EXPECT_EQ(Bits(world.GetTerrainHeightAt(8.0f, 8.0f)),
              Bits(fresh.GetTerrainHeightAt(8.0f, 8.0f)));
}

TEST(HeightSamplingCompatibility, ActivationUsesSavedRevision) {
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.base_amplitude = 0.0f;
    params.height_offset = 47.0f;
    params.cliffs_enabled = true;
    params.cliff_threshold = -2.0f; // every sampled column terraces
    params.cliff_step = 48.0f;      // 47 m snaps to 48 m, crossing chunk Y
    const SHIELD_WorldSystem legacy(nullptr, nullptr, params, 1337);
    params.cliffs_enabled = false;
    const SHIELD_WorldSystem historical_reference(nullptr, nullptr, params, 1337);
    params.cliffs_enabled = true;
    params.surface_sampling_revision = Luminumbra::Systems::kSurfaceSamplingRevision;
    const SHIELD_WorldSystem current(nullptr, nullptr, params, 1337);
    const auto old_chunks = legacy.GetInitialChunkLoadList(Vec3(0.0f));
    EXPECT_EQ(old_chunks, historical_reference.GetInitialChunkLoadList(Vec3(0.0f)));
    const auto new_chunks = current.GetInitialChunkLoadList(Vec3(0.0f));
    EXPECT_NE(new_chunks, old_chunks);
    ASSERT_FALSE(old_chunks.empty());
    ASSERT_FALSE(new_chunks.empty());
    EXPECT_EQ(old_chunks.front().y, 2);
    EXPECT_EQ(new_chunks.front().y, 3);

    // The selector changes residency and vertex materials, not terrain bytes or
    // generated-tile identity. Existing presets retain their identity verbatim.
    auto old_params = params;
    old_params.surface_sampling_revision = 1;
    EXPECT_EQ(Luminumbra::World::ComputeTerrainParamsHash(old_params, 1337),
              Luminumbra::World::ComputeTerrainParamsHash(params, 1337));

    const std::array<Vec3, 5> positions{Vec3(0.0f, 46.0f, 0.0f),
                                        Vec3(0.0f, 47.25f, 0.0f),
                                        Vec3(0.0f, 48.25f, 0.0f),
                                        Vec3(0.0f, 49.0f, 0.0f),
                                        Vec3(0.0f, 44.0f, 0.0f)};
    std::array<Luminumbra::u32, 5> legacy_materials{};
    auto reference_materials = legacy_materials;
    auto current_materials = legacy_materials;
    legacy.ClassifyVertexMaterials(positions.data(), positions.size(), legacy_materials.data());
    historical_reference.ClassifyVertexMaterials(
        positions.data(), positions.size(), reference_materials.data());
    current.ClassifyVertexMaterials(positions.data(), positions.size(), current_materials.data());
    EXPECT_EQ(legacy_materials, reference_materials);
    EXPECT_NE(current_materials, legacy_materials);
    for (std::size_t i = 0; i < positions.size(); ++i) {
        const auto& position = positions[i];
        auto material = current.SampleWorldGenLayers(position - Vec3(0.0f, 0.25f, 0.0f)).material;
        if (material == Luminumbra::MaterialType::Air ||
            material == Luminumbra::MaterialType::Water) {
            material =
                current.SurfaceMaterialForColumn(position.y - 0.1f,
                                                 current.GetTerrainHeightAt(position.x, position.z),
                                                 current.BiomeIdAt(position.x, position.z),
                                                 false);
        }
        EXPECT_EQ(current_materials[i], static_cast<Luminumbra::u32>(material));
    }
}

TEST(HeightSamplingContract, ChunkMaterialWorkReusesHydroLakeAndFeatureCells) {
    const auto loaded = Luminumbra::world::LoadTerrainPreset(PresetPath("default"));
    ASSERT_TRUE(loaded.ok);
    auto params = loaded.params;
    params.surface_sampling_revision = Luminumbra::Systems::kSurfaceSamplingRevision;
    params.hydro_enabled = true;
    params.hydro_iterations = 1;
    params.lakes_enabled = true;
    params.surface_breaks_enabled = true;
    params.cliffs_enabled = true;
    const SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    for (const float seam : {-1536.0f, 1536.0f}) {
        // 1536 is shared by the 512 m hydro and 768 m lake grids. Start just
        // before it to exercise all four regions, including negative floor division.
        std::vector<Vec3> positions;
        for (int z = 0; z <= Luminumbra::CHUNK_SIZE_Z; ++z) {
            for (int x = 0; x <= Luminumbra::CHUNK_SIZE_X; ++x) {
                const float wx = seam - 0.25f + static_cast<float>(x);
                const float wz = seam - 0.25f + static_cast<float>(z);
                const float height = world.GetTerrainHeightAt(wx, wz);
                for (const float dy : {-1.0f, 0.0f, 1.0f})
                    positions.emplace_back(wx, height + dy, wz);
            }
        }
        std::vector<Luminumbra::u32> materials(positions.size());
        SHIELD_WorldSystem::MaterialSamplingWork work;
        world.ClassifyVertexMaterials(positions.data(), positions.size(), materials.data(), &work);
        EXPECT_EQ(work.shaped_columns, positions.size() / 3);
        EXPECT_EQ(work.hydro_regions, 4u);
        EXPECT_EQ(work.lake_cells, 4u);
        EXPECT_GT(work.surface_break_cells, 0u);
        EXPECT_LE(work.surface_break_cells, 16u);
        for (std::size_t i = 0; i < positions.size(); ++i) {
            const auto& position = positions[i];
            auto expected = world.SampleWorldGenLayers(position - Vec3(0.0f, 0.25f, 0.0f)).material;
            if (expected == Luminumbra::MaterialType::Air ||
                expected == Luminumbra::MaterialType::Water) {
                expected = world.SurfaceMaterialForColumn(
                    position.y - 0.1f,
                    world.GetTerrainHeightAt(position.x, position.z),
                    world.BiomeIdAt(position.x, position.z),
                    world.RiverInfluenceAt(position.x, position.z) > 0.25f);
            }
            EXPECT_EQ(materials[i], static_cast<Luminumbra::u32>(expected));
        }
    }
    SHIELD_WorldSystem::MaterialSamplingWork empty{1, 1, 1, 1};
    world.ClassifyVertexMaterials(nullptr, 0, nullptr, &empty);
    EXPECT_EQ(empty.shaped_columns, 0u);
    EXPECT_EQ(empty.hydro_regions, 0u);
    EXPECT_EQ(empty.lake_cells, 0u);
    EXPECT_EQ(empty.surface_break_cells, 0u);
    const auto too_many = static_cast<std::size_t>(std::numeric_limits<int>::max()) + 1;
    EXPECT_THROW(world.ClassifyVertexMaterials(nullptr, too_many, nullptr), std::length_error);
}

TEST(HeightSamplingContract, MaterialWorkingSetsEvictWithoutChangingResults) {
    const auto loaded = Luminumbra::world::LoadTerrainPreset(PresetPath("default"));
    ASSERT_TRUE(loaded.ok);
    auto params = loaded.params;
    params.surface_sampling_revision = Luminumbra::Systems::kSurfaceSamplingRevision;
    params.hydro_enabled = true;
    params.hydro_iterations = 1;
    params.lakes_enabled = true;
    params.surface_breaks_enabled = true;
    const SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    std::array<Vec3, 8> positions{};
    // Seven distinct working sets overflow each fixed cache; revisit the first.
    for (std::size_t i = 0; i + 1 < positions.size(); ++i) {
        const float coordinate = static_cast<float>(i) * 1536.0f + 0.25f;
        positions[i] = Vec3(coordinate, 50.0f, coordinate);
    }
    positions.back() = positions.front();
    std::array<Luminumbra::u32, 8> batched{}, scalar{};
    world.ClassifyVertexMaterials(positions.data(), positions.size(), batched.data());
    for (std::size_t i = 0; i < positions.size(); ++i)
        world.ClassifyVertexMaterials(&positions[i], 1, &scalar[i]);
    EXPECT_EQ(batched, scalar);
    EXPECT_EQ(batched.front(), batched.back());

    // More distinct columns than the direct-mapped cache can retain force
    // collisions. Revisit them with distinct vertical offsets and fresh-call oracles.
    std::vector<Vec3> collision_positions;
    for (int z = 0; z < 16; ++z) {
        for (int x = 0; x < 32; ++x) {
            const float wx = static_cast<float>(x) * 0.5f + 0.25f;
            const float wz = static_cast<float>(z) + 0.25f;
            collision_positions.emplace_back(wx, world.GetTerrainHeightAt(wx, wz), wz);
        }
    }
    for (std::size_t i = 0; i < 512; ++i)
        collision_positions.push_back(collision_positions[i] + Vec3(0.0f, -4.0f, 0.0f));
    std::vector<Luminumbra::u32> collision_materials(collision_positions.size());
    SHIELD_WorldSystem::MaterialSamplingWork collision_work;
    world.ClassifyVertexMaterials(collision_positions.data(),
                                  collision_positions.size(),
                                  collision_materials.data(),
                                  &collision_work);
    EXPECT_GT(collision_work.shaped_columns, 512u);
    EXPECT_LE(collision_work.shaped_columns, collision_positions.size());
    for (std::size_t i = 0; i < collision_positions.size(); ++i) {
        Luminumbra::u32 expected = 0;
        world.ClassifyVertexMaterials(&collision_positions[i], 1, &expected);
        EXPECT_EQ(collision_materials[i], expected);
    }

    params.hydro_enabled = false;
    params.lakes_enabled = false;
    params.surface_breaks_enabled = false;
    const SHIELD_WorldSystem disabled(nullptr, nullptr, params, 1337);
    SHIELD_WorldSystem::MaterialSamplingWork work;
    disabled.ClassifyVertexMaterials(positions.data(), positions.size(), batched.data(), &work);
    EXPECT_EQ(work.hydro_regions, 0u);
    EXPECT_EQ(work.lake_cells, 0u);
    EXPECT_EQ(work.surface_break_cells, 0u);
}

TEST(HeightSamplingContract, RejectsOversizeBeforeReadingOrAllocating) {
    const auto params = Luminumbra::Test::SyntheticTerrainParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    const auto too_many = static_cast<std::size_t>(std::numeric_limits<int>::max()) + 1;
    EXPECT_THROW(world.ComputeShapedHeightsAtPositions(nullptr, nullptr, too_many, nullptr),
                 std::length_error);
    EXPECT_NO_THROW(world.ComputeShapedHeightsAtPositions(nullptr, nullptr, 0, nullptr));
}
} // namespace
