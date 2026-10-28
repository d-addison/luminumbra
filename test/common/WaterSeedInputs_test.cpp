#include "../fixtures/TerrainFixtureParams.h"
#include "luminumbra/core/Types.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/core/WaterAttribution.h"
#include "luminumbra_common/core/WaterComponents.h"
#include "luminumbra_common/core/WaterSeedInputs.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WaterSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/WorldStreamingState.h"
#include <entt/entity/registry.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
using namespace Luminumbra;
using Systems::SHIELD_WorldSystem;
using Systems::WaterSystem;

Systems::TerrainGenParams SeedParams() {
    auto params = Test::SyntheticTerrainParams();
    params.lakes_enabled = true;
    params.height_offset = -12.0f;
    return params;
}

// A fixed resident fixture: no streaming, clock, rendering or filesystem work.
class SeedFixture {
private:
    SHIELD_WorldSystem m_world{nullptr, nullptr, SeedParams(), 777};
    WaterSystem m_water{nullptr, &m_world};
    entt::basic_registry<> m_registry;
    std::unordered_map<ChunkID, std::shared_ptr<Chunk>> m_chunks;

public:
    explicit SeedFixture(int count = 1, JobSystem* jobs = nullptr)
        : m_water(jobs, &m_world) {
        for (int i = 0; i < count; ++i) {
            auto chunk = std::make_shared<Chunk>(IVec3(i - 4, 0, -2));
            EXPECT_TRUE(m_world.adopt_streamed_chunk(chunk));
            m_chunks.emplace(chunk->get_id(), std::move(chunk));
        }
    }

    SHIELD_WorldSystem& world() {
        return m_world;
    }
    const SHIELD_WorldSystem& world() const {
        return m_world;
    }
    WaterSystem& water() {
        return m_water;
    }
    const auto& chunks() const {
        return m_chunks;
    }

    void prepare(Chunk& chunk, bool ready = true) {
        auto inputs = std::make_shared<HighWaterSeedInputs>();
        inputs->generation = {m_world.water_seed_terrain_generation(),
                              chunk.water_seed_edit_generation()};
        if (ready) {
            const auto scope = m_world.acquire_worldgen_sample_scope();
            ComputeHighWaterSeedInputs(*inputs, chunk.get_coords(), m_world);
        } else {
            // Unpublished storage must never be consumed, even if it contains data.
            inputs->terrain.fill(10000.0f);
            inputs->rest.fill(-10000.0f);
        }
        chunk.stage_water_seed_inputs(std::move(inputs));
    }

    void tick() {
        m_water.update(m_registry, m_chunks);
    }
};

void ExpectSameWater(const SeedFixture& a, const SeedFixture& b) {
    EXPECT_EQ(a.world().debug_water_state_hash().hash, b.world().debug_water_state_hash().hash);
    EXPECT_EQ(a.world().debug_water_state_hash().water_chunks,
              b.world().debug_water_state_hash().water_chunks);
    for (const auto& [id, chunk] : a.chunks()) {
        const auto& other = *b.chunks().at(id);
        EXPECT_EQ(chunk->has_water_sim.load(), other.has_water_sim.load());
        EXPECT_EQ(chunk->water_bed_mm, other.water_bed_mm);
        EXPECT_EQ(chunk->water_depth_mm, other.water_depth_mm);
        EXPECT_EQ(chunk->water_edge_flux, other.water_edge_flux);
        ASSERT_EQ(chunk->water_rest_level.size(), other.water_rest_level.size());
        ASSERT_EQ(chunk->water_sim_terrain_height.size(), other.water_sim_terrain_height.size());
        for (std::size_t i = 0; i < chunk->water_rest_level.size(); ++i) {
            EXPECT_EQ(std::bit_cast<std::uint32_t>(chunk->water_rest_level[i]),
                      std::bit_cast<std::uint32_t>(other.water_rest_level[i]));
            EXPECT_EQ(std::bit_cast<std::uint32_t>(chunk->water_sim_terrain_height[i]),
                      std::bit_cast<std::uint32_t>(other.water_sim_terrain_height[i]));
        }
    }
}

void ExpectInlineSeed(SeedFixture& fixture, int resolution) {
    ASSERT_EQ(fixture.chunks().size(), 1u);
    const auto& chunk = *fixture.chunks().begin()->second;
    ASSERT_EQ(chunk.current_water_resolution.load(), resolution);
    const auto cells = static_cast<std::size_t>(resolution) * static_cast<std::size_t>(resolution);
    ASSERT_EQ(chunk.water_bed_mm.size(), cells);
    ASSERT_EQ(chunk.water_rest_level.size(), cells);
    ASSERT_EQ(chunk.water_sim_terrain_height.size(), cells);
    ASSERT_EQ(chunk.water_level_data.size(), cells);
    ASSERT_EQ(chunk.water_flow_data.size(), cells);
    EXPECT_EQ(fixture.water().cells_stepped_last_update(), 0u);
    std::vector<std::int32_t> beds;
    std::vector<std::int32_t> depths;
    const auto coords = chunk.get_coords();
    for (int z = 0; z < resolution; ++z) {
        for (int x = 0; x < resolution; ++x) {
            const float wx =
                static_cast<float>(coords.x * CHUNK_SIZE_X) +
                (static_cast<float>(x) + 0.5f) * (CHUNK_SIZE_X / static_cast<float>(resolution));
            const float wz =
                static_cast<float>(coords.z * CHUNK_SIZE_Z) +
                (static_cast<float>(z) + 0.5f) * (CHUNK_SIZE_Z / static_cast<float>(resolution));
            const float rest = fixture.world().WaterLevelAt(wx, wz);
            const float terrain = fixture.world().GetTerrainHeightAt(wx, wz);
            const auto cell = static_cast<std::size_t>(z) * static_cast<std::size_t>(resolution) +
                              static_cast<std::size_t>(x);
            beds.push_back(static_cast<std::int32_t>(std::lround(terrain * 1000)));
            const long depth = std::lround(static_cast<double>(rest - terrain) * 1000);
            depths.push_back(static_cast<std::int32_t>(depth > 0 ? depth : 0));
            EXPECT_EQ(std::bit_cast<std::uint32_t>(chunk.water_rest_level[cell]),
                      std::bit_cast<std::uint32_t>(rest));
            EXPECT_EQ(std::bit_cast<std::uint32_t>(chunk.water_level_data[cell]),
                      std::bit_cast<std::uint32_t>(rest));
            EXPECT_FLOAT_EQ(chunk.water_flow_data[cell].x, 0.0f);
            EXPECT_FLOAT_EQ(chunk.water_flow_data[cell].y, 0.0f);
            EXPECT_EQ(std::bit_cast<std::uint32_t>(chunk.water_sim_terrain_height[cell]),
                      std::bit_cast<std::uint32_t>(terrain));
        }
    }
    EXPECT_EQ(chunk.water_bed_mm, beds);
    EXPECT_EQ(chunk.water_depth_mm, depths);
    std::uint64_t expected_hash = 1469598103934665603ull;
    const auto mix = [&expected_hash](std::uint64_t value) {
        expected_hash ^= value;
        expected_hash *= 1099511628211ull;
    };
    mix(static_cast<std::uint64_t>(chunk.get_id()));
    for (const auto depth : depths)
        mix(static_cast<std::uint32_t>(depth));
    for (const auto bed : beds)
        mix(static_cast<std::uint32_t>(bed));
    EXPECT_EQ(fixture.world().debug_water_state_hash().hash, expected_hash);
}

TEST(WaterSeedInputs, HighCellsMatchOriginalInlineBitsIncludingEditedTerrain) {
    SeedFixture fixture;
    auto& chunk = *fixture.chunks().begin()->second;
    // Match saved/edited terrain provenance. High's historical inline sampler
    // remains analytic even when the voxel/heightmap payload has been edited.
    fixture.world().GenerateChunkData(chunk, 1);
    ASSERT_FALSE(chunk.heightmap_data.empty());
    ASSERT_FALSE(chunk.sdf_data.empty());
    for (int edit = 0; edit < 2; ++edit) {
        if (edit != 0) {
            constexpr std::size_t center = 8 + 8 * 17 + 8 * 17 * 17;
            const bool fill = chunk.sdf_data[center] >= 0;
            const auto coords = chunk.get_coords();
            const Vec3 center_pos(static_cast<float>(coords.x * CHUNK_SIZE_X + 8),
                                  8.0f,
                                  static_cast<float>(coords.z * CHUNK_SIZE_Z + 8));
            ASSERT_EQ(fixture.world().EditTerrainVoxel(center_pos, 2.0f, fill, nullptr), 1);
        }
        fixture.prepare(chunk);
        const auto& inputs = *chunk.water_seed_inputs();
        ASSERT_TRUE(inputs.ready.load(std::memory_order_acquire));
        EXPECT_EQ(inputs.generation.edit, chunk.water_seed_edit_generation());
        const auto coords = chunk.get_coords();
        // Independent oracle: the pre-change inline arithmetic, in cell order.
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                const float wx = static_cast<float>(coords.x * CHUNK_SIZE_X) +
                                 (static_cast<float>(x) + 0.5f) * (CHUNK_SIZE_X / 16.0f);
                const float wz = static_cast<float>(coords.z * CHUNK_SIZE_Z) +
                                 (static_cast<float>(z) + 0.5f) * (CHUNK_SIZE_Z / 16.0f);
                const auto cell = static_cast<std::size_t>(z) * 16 + static_cast<std::size_t>(x);
                EXPECT_EQ(std::bit_cast<std::uint32_t>(inputs.rest[cell]),
                          std::bit_cast<std::uint32_t>(fixture.world().WaterLevelAt(wx, wz)));
                EXPECT_EQ(std::bit_cast<std::uint32_t>(inputs.terrain[cell]),
                          std::bit_cast<std::uint32_t>(fixture.world().GetTerrainHeightAt(wx, wz)));
            }
        }
    }
}

TEST(WaterSeedInputs, ScalarQueriesKeepZMajorXMinorAndRestBeforeTerrainOrder) {
    struct Query {
        float x;
        float z;
        bool terrain;
    };
    class RecordingSampler {
        mutable std::vector<Query> m_queries;

    public:
        const auto& queries() const {
            return m_queries;
        }
        float WaterLevelAt(float x, float z) const {
            m_queries.push_back({x, z, false});
            return 0;
        }
        float GetTerrainHeightAt(float x, float z) const {
            m_queries.push_back({x, z, true});
            return -1;
        }
    };
    const RecordingSampler sampler;
    HighWaterSeedInputs inputs;
    ComputeHighWaterSeedInputs(inputs, IVec3(-2, 0, 3), sampler);
    ASSERT_EQ(sampler.queries().size(), 512u);
    for (std::size_t cell = 0; cell < 256; ++cell) {
        const float x = -32.0f + (static_cast<float>(cell % 16) + 0.5f);
        const auto row = cell / 16;
        const float z = 48.0f + (static_cast<float>(row) + 0.5f);
        EXPECT_FLOAT_EQ(sampler.queries()[2 * cell].x, x);
        EXPECT_FLOAT_EQ(sampler.queries()[2 * cell].z, z);
        EXPECT_FALSE(sampler.queries()[2 * cell].terrain);
        EXPECT_FLOAT_EQ(sampler.queries()[2 * cell + 1].x, x);
        EXPECT_FLOAT_EQ(sampler.queries()[2 * cell + 1].z, z);
        EXPECT_TRUE(sampler.queries()[2 * cell + 1].terrain);
    }
}

TEST(WaterSeedInputs, StaleEditAndTerrainGenerationsAreDiscarded) {
    for (int mutation = 0; mutation < 4; ++mutation) {
        SeedFixture prepared;
        SeedFixture fallback;
        auto& chunk = *prepared.chunks().begin()->second;
        if (mutation == 0) {
            prepared.world().GenerateChunkData(chunk, 1);
            fallback.world().GenerateChunkData(*fallback.chunks().begin()->second, 1);
        }
        prepared.prepare(chunk);
        // Poison a completed old-generation result: accepting it must fail loudly.
        auto stale_inputs = chunk.take_water_seed_inputs();
        stale_inputs->terrain.fill(10000.0f);
        chunk.stage_water_seed_inputs(std::move(stale_inputs));
        if (mutation == 0) {
            constexpr std::size_t center = 8 + 8 * 17 + 8 * 17 * 17;
            const bool fill = chunk.sdf_data[center] >= 0;
            const auto coords = chunk.get_coords();
            const Vec3 center_pos(static_cast<float>(coords.x * CHUNK_SIZE_X + 8),
                                  8.0f,
                                  static_cast<float>(coords.z * CHUNK_SIZE_Z + 8));
            ASSERT_EQ(prepared.world().EditTerrainVoxel(center_pos, 2.0f, fill, nullptr), 1);
            ASSERT_EQ(fallback.world().EditTerrainVoxel(center_pos, 2.0f, fill, nullptr), 1);
        } else if (mutation == 3) {
            chunk.mark_voxel_data_dirty();
            fallback.chunks().begin()->second->mark_voxel_data_dirty();
            // Regeneration resets the far-LOD voxel revision, but must not
            // revive a water payload produced before the intervening edit.
            chunk.mark_sdf_generated_current_params();
            fallback.chunks().begin()->second->mark_sdf_generated_current_params();
            EXPECT_EQ(chunk.voxel_revision(), 0u);
        } else if (mutation == 1) {
            auto params = SeedParams();
            params.height_offset += 5.0f;
            prepared.world().set_params(params);
            fallback.world().set_params(params);
        } else {
            prepared.world().set_seed(778);
            fallback.world().set_seed(778);
        }
        prepared.tick();
        fallback.tick();
        EXPECT_EQ(prepared.water().dbg_seed_attribution().stale_columns, 1u);
        EXPECT_EQ(prepared.water().dbg_seed_attribution().fallback_columns, 1u);
        EXPECT_EQ(prepared.water().dbg_seed_attribution().precomputed_columns, 0u);
        EXPECT_FALSE(chunk.water_seed_inputs());
        ExpectSameWater(prepared, fallback);
    }
}

TEST(WaterSeedInputs, HighInitializationPreservesInlineRoundingAndCellLayout) {
    for (const bool precompute : {false, true}) {
        SeedFixture fixture;
        auto& chunk = *fixture.chunks().begin()->second;
        chunk.mark_voxel_data_dirty();
        chunk.is_water_sleeping.store(true);
        if (precompute)
            fixture.prepare(chunk);
        fixture.tick();
        ExpectInlineSeed(fixture, 16);
    }
}

// A 32-cell session grid is unreachable: the setter refuses it, the default is
// High, and session creation/loading select Medium or High. Exercise refusal via
// the public API; the scalar wiring suite directly checks the defensive predicate
// and seed_chunk_water guard for unsupported resolutions without selecting them.
TEST(WaterSeedInputs, RejectedResolutionBootAndLiveKeepConfiguredGridAndInlineHash) {
    JobSystem jobs;
    jobs.startup(2);
    for (const int resolution : {2, 4, 8, 16}) {
        for (const bool boot : {false, true}) {
            for (const bool staged : {false, true}) {
                SeedFixture accelerated(1, &jobs);
                SeedFixture inline_path;
                accelerated.water().SetSimResolution(resolution);
                inline_path.water().SetSimResolution(resolution);
                ASSERT_EQ(accelerated.water().sim_resolution(), resolution);
                accelerated.water().SetSimResolution(32);
                ASSERT_EQ(accelerated.water().sim_resolution(), resolution);
                ASSERT_EQ(inline_path.water().sim_resolution(), resolution);
                accelerated.water().SetBootSettleMode(boot);
                auto& chunk = *accelerated.chunks().begin()->second;
                chunk.is_water_sleeping.store(true);
                inline_path.chunks().begin()->second->is_water_sleeping.store(true);
                if (staged) {
                    accelerated.prepare(chunk);
                    auto inputs = chunk.take_water_seed_inputs();
                    // Even a published payload claiming the refused size is invalid.
                    inputs->resolution = 32;
                    inputs->terrain.fill(10000.0f);
                    inputs->rest.fill(-10000.0f);
                    chunk.stage_water_seed_inputs(std::move(inputs));
                }
                accelerated.tick();
                inline_path.tick();
                EXPECT_FALSE(chunk.water_seed_inputs());
                ExpectSameWater(accelerated, inline_path);
                ExpectInlineSeed(accelerated, resolution);
                ExpectInlineSeed(inline_path, resolution);
                // An empty initialization set preserves the seed on a later update.
                const auto seeded_hash = accelerated.world().debug_water_state_hash().hash;
                accelerated.tick();
                inline_path.tick();
                EXPECT_EQ(accelerated.world().debug_water_state_hash().hash, seeded_hash);
                ExpectSameWater(accelerated, inline_path);
            }
        }
    }
    jobs.shutdown();
}

TEST(WaterSeedInputs, UnpublishedPartialAndWrongResolutionInputsFallBackInBootAndLive) {
    JobSystem jobs;
    jobs.startup(2);
    for (const bool boot : {false, true}) {
        for (int payload = 0; payload < 4; ++payload) {
            SeedFixture prepared(1, &jobs);
            SeedFixture inline_path;
            prepared.water().SetBootSettleMode(boot);
            auto& chunk = *prepared.chunks().begin()->second;
            chunk.is_water_sleeping.store(true);
            inline_path.chunks().begin()->second->is_water_sleeping.store(true);
            prepared.prepare(chunk, false);
            auto inputs = chunk.take_water_seed_inputs();
            if (payload == 0) {
                // Unpublished data must not be read even if its resolution matches.
                inputs->resolution = 16;
            } else if (payload == 1) {
                const auto scope = prepared.world().acquire_worldgen_sample_scope();
                ComputeWaterSeedInputs(*inputs, chunk.get_coords(), 8, prepared.world());
                ASSERT_TRUE(inputs->ready_for(8));
            } else if (payload == 2) {
                const std::array<float, 63> partial{};
                ComputeWaterSeedInputs(*inputs, chunk.get_coords(), 8, prepared.world(), partial);
                EXPECT_FALSE(inputs->ready.load(std::memory_order_acquire));
            } else {
                // Published storage without resolution metadata is also invalid.
                inputs->ready.store(true, std::memory_order_release);
            }
            ASSERT_FALSE(inputs->ready_for(16));
            chunk.stage_water_seed_inputs(std::move(inputs));
            prepared.tick();
            inline_path.tick();
            EXPECT_FALSE(chunk.water_seed_inputs());
            EXPECT_EQ(prepared.water().dbg_seed_attribution().precomputed_columns, 0u);
            EXPECT_EQ(prepared.water().dbg_seed_attribution().fallback_columns, 1u);
            ExpectSameWater(prepared, inline_path);
            ExpectInlineSeed(prepared, 16);
        }
    }
    jobs.shutdown();
}

TEST(WaterSeedInputs, LakeDisabledBootAndLiveRetireInputsAndKeepInlineHash) {
    JobSystem jobs;
    jobs.startup(2);
    for (const bool boot : {false, true}) {
        for (const int resolution : {2, 4, 8, 16}) {
            SeedFixture prepared(1, &jobs);
            SeedFixture inline_path;
            auto params = SeedParams();
            params.lakes_enabled = false;
            prepared.world().set_params(params);
            inline_path.world().set_params(params);
            prepared.water().SetSimResolution(resolution);
            ASSERT_EQ(prepared.water().sim_resolution(), resolution);
            inline_path.water().SetSimResolution(resolution);
            ASSERT_EQ(inline_path.water().sim_resolution(), resolution);
            prepared.water().SetBootSettleMode(boot);
            auto& chunk = *prepared.chunks().begin()->second;
            chunk.is_water_sleeping.store(true);
            inline_path.chunks().begin()->second->is_water_sleeping.store(true);
            prepared.prepare(chunk, false);
            prepared.tick();
            inline_path.tick();
            EXPECT_FALSE(chunk.water_seed_inputs());
            EXPECT_EQ(prepared.water().dbg_seed_attribution().fallback_columns, 0u);
            EXPECT_EQ(prepared.water().dbg_seed_attribution().precomputed_columns, 0u);
            ExpectSameWater(prepared, inline_path);
            ExpectInlineSeed(prepared, resolution);
        }
    }
    jobs.shutdown();
}

TEST(WaterSeedInputs, InvalidSessionResolutionsLeaveTheConfiguredGridUnchanged) {
    for (const int configured : {2, 4, 8, 16}) {
        SeedFixture fixture;
        fixture.water().SetSimResolution(configured);
        ASSERT_EQ(fixture.water().sim_resolution(), configured);
        for (const int resolution : {std::numeric_limits<int>::min(),
                                     -1,
                                     0,
                                     1,
                                     3,
                                     17,
                                     31,
                                     32,
                                     33,
                                     64,
                                     std::numeric_limits<int>::max()}) {
            fixture.water().SetSimResolution(resolution);
            EXPECT_EQ(fixture.water().sim_resolution(), configured);
        }
    }
}

TEST(WaterSeedInputs, SavedUltraChunkRestoresAndMigratesWithoutSelectingUltraSession) {
    WorldStreamingState saved;
    const auto chunk = saved.get_or_create_chunk(IVec3(0));
    ASSERT_TRUE(chunk);
    chunk->has_water_sim.store(true);
    chunk->current_water_resolution.store(32);
    // The saved grid is bounded to Ultra's 1024 cells; no solver step runs at 32.
    constexpr std::size_t kSavedCells = 32u * 32u;
    chunk->water_level_data.assign(kSavedCells, 2.0f);
    chunk->water_flow_data.assign(kSavedCells, Vec2(0.0f));
    chunk->water_sim_terrain_height.assign(kSavedCells, 1.0f);
    chunk->water_bed_mm.assign(kSavedCells, 1000);
    chunk->water_depth_mm.assign(kSavedCells, 1000);
    chunk->water_edge_flux.assign(2u * kSavedCells, 0);
    const auto snapshot = Persistence::SerializeWorldStreamingStateSnapshotJson(saved);
    for (const int resolution : {8, 16}) {
        WorldStreamingState restored;
        std::vector<std::string> errors;
        ASSERT_TRUE(Persistence::LoadWorldStreamingStateSnapshotJson(snapshot, restored, errors));
        ASSERT_TRUE(errors.empty());
        const auto chunks = restored.snapshot_chunks();
        ASSERT_EQ(chunks.size(), 1u);
        ASSERT_TRUE(chunks.front());
        const auto& loaded = *chunks.front();
        ASSERT_EQ(loaded.current_water_resolution.load(), 32);
        EXPECT_EQ(loaded.water_depth_mm, chunk->water_depth_mm);
        SHIELD_WorldSystem world(nullptr, nullptr, SeedParams(), 777);
        WaterSystem water(nullptr, &world);
        water.SetSimResolution(resolution);
        ASSERT_EQ(water.sim_resolution(), resolution);
        world.SetWaterSystem(&water);
        ASSERT_TRUE(world.adopt_streamed_chunk(chunks.front()));
        EXPECT_EQ(world.MigrateWaterSimResolution(), 1u);
        EXPECT_EQ(water.sim_resolution(), resolution);
        EXPECT_EQ(loaded.current_water_resolution.load(), resolution);
        const auto cells =
            static_cast<std::size_t>(resolution) * static_cast<std::size_t>(resolution);
        EXPECT_EQ(loaded.water_depth_mm, (std::vector<std::int32_t>(cells, 1000)));
        EXPECT_EQ(loaded.water_bed_mm, (std::vector<std::int32_t>(cells, 1000)));
        EXPECT_EQ(world.MigrateWaterSimResolution(), 0u);
    }
}

// An independent Q8 coordinate oracle: round the whole coordinate first, then
// form its four weights. Signed rounding is expressed through quotient/remainder.
std::vector<std::int32_t>
MigratedWaterOracle(const std::vector<std::int32_t>& source, int old_res, int new_res) {
    std::vector<std::int32_t> result(static_cast<std::size_t>(new_res * new_res));
    for (int z = 0; z < new_res; ++z) {
        for (int x = 0; x < new_res; ++x) {
            const auto coordinate = [=](int value) {
                return (2 * value * (old_res - 1) * 256 + new_res - 1) / (2 * (new_res - 1));
            };
            const int qx = coordinate(x);
            const int qz = coordinate(z);
            std::int64_t sum = 0;
            for (int dz = 0; dz != 2; ++dz) {
                for (int dx = 0; dx != 2; ++dx) {
                    const int sx = std::min(old_res - 1, qx / 256 + dx);
                    const int sz = std::min(old_res - 1, qz / 256 + dz);
                    const int wx = dx ? qx % 256 : 256 - qx % 256;
                    const int wz = dz ? qz % 256 : 256 - qz % 256;
                    sum += static_cast<std::int64_t>(
                               source[static_cast<std::size_t>(sz * old_res + sx)]) *
                           wx * wz;
                }
            }
            result[static_cast<std::size_t>(z * new_res + x)] =
                static_cast<std::int32_t>(sum / 65536 + (sum % 65536 >= 32768   ? 1
                                                         : sum % 65536 < -32768 ? -1
                                                                                : 0));
        }
    }
    return result;
}

TEST(WaterSeedInputs, MigrationKeepsInt32ExtremesInBothDirections) {
    constexpr std::int32_t kMax = std::numeric_limits<std::int32_t>::max();
    constexpr std::int32_t kMin = std::numeric_limits<std::int32_t>::min();
    for (const int old_res : {2, 8, 16}) {
        for (const int new_res : {2, 8, 16}) {
            if (old_res == new_res) {
                continue;
            }
            SCOPED_TRACE(::testing::Message() << old_res << " -> " << new_res);
            for (const auto bed : {kMin, kMax}) {
                SCOPED_TRACE(bed);
                SeedFixture fixture;
                fixture.water().SetSimResolution(old_res);
                fixture.tick();
                auto& chunk = *fixture.chunks().begin()->second;
                const auto old_cells = static_cast<std::size_t>(old_res * old_res);
                ASSERT_EQ(chunk.water_bed_mm.size(), old_cells);
                ASSERT_EQ(chunk.water_depth_mm.size(), old_cells);
                chunk.water_bed_mm.assign(old_cells, bed);
                chunk.water_depth_mm.assign(old_cells, kMax);
                fixture.water().SetSimResolution(new_res);
                ASSERT_EQ(fixture.water().MigrateChunksToSimResolution(fixture.chunks()), 1u);
                const auto new_cells = static_cast<std::size_t>(new_res * new_res);
                // In all six resolution pairs, bilinear weights sum to 65536.
                // A constant grid stays constant, even at either signed extreme;
                // only depths have a nonnegative contract. Mirrors use a wide sum.
                EXPECT_EQ(chunk.water_bed_mm, std::vector<std::int32_t>(new_cells, bed));
                EXPECT_EQ(chunk.water_depth_mm, std::vector<std::int32_t>(new_cells, kMax));
                const float surface =
                    static_cast<float>(static_cast<std::int64_t>(bed) + kMax) / 1000.0f;
                EXPECT_EQ(chunk.water_level_data, std::vector<float>(new_cells, surface));
                // Exercise the migrated bed through the real sink and mirror passes,
                // including 0 - INT32_MIN (one beyond the representable depth range).
                fixture.water().SetHydrology(true, 0, 0);
                chunk.is_water_sleeping.store(false);
                fixture.tick();
                EXPECT_EQ(chunk.water_depth_mm, std::vector<std::int32_t>(new_cells, kMax));
                EXPECT_EQ(chunk.water_level_data, std::vector<float>(new_cells, surface));
                EXPECT_EQ(fixture.water().dbg_last_sink_mm(), 0);
                EXPECT_TRUE(fixture.water().dbg_mass_ok());
            }
        }
    }
}

TEST(WaterSeedInputs, SolverBoundsSourcesNegativeDepthsAndActivityDelta) {
    constexpr auto kMax = std::numeric_limits<std::int32_t>::max();
    constexpr auto kMin = std::numeric_limits<std::int32_t>::min();
    for (const int resolution : {2, 8, 16}) {
        SeedFixture fixture;
        fixture.water().SetSimResolution(resolution);
        fixture.tick();
        auto& chunk = *fixture.chunks().begin()->second;
        const auto cells = chunk.water_depth_mm.size();
        for (const auto initial : {kMin, -1, 0, kMax - 1, kMax}) {
            chunk.water_bed_mm.assign(cells, kMax);
            chunk.water_depth_mm.assign(cells, initial);
            chunk.water_edge_flux.assign(2 * cells, 0);
            fixture.water().SetHydrology(true, kMax, 0);
            chunk.is_water_sleeping.store(false);
            fixture.tick();
            EXPECT_EQ(chunk.water_depth_mm, std::vector<std::int32_t>(cells, kMax));
            EXPECT_FLOAT_EQ(chunk.max_water_delta_last_tick,
                            static_cast<float>(static_cast<std::int64_t>(kMax) - initial) /
                                1000.0f);
            EXPECT_TRUE(fixture.water().dbg_mass_ok());
            EXPECT_EQ(fixture.water().dbg_last_source_mm(),
                      (static_cast<std::int64_t>(kMax) - initial) *
                          static_cast<std::int64_t>(cells));
        }
        for (const auto initial : {kMax - 24, kMax - 23, kMax}) {
            chunk.water_depth_mm.assign(cells, initial);
            chunk.water_src_mm.assign(cells, 24);
            fixture.water().SetHydrology(false, 0, 0);
            chunk.is_water_sleeping.store(false);
            fixture.tick();
            EXPECT_EQ(chunk.water_depth_mm, std::vector<std::int32_t>(cells, kMax));
            EXPECT_EQ(fixture.water().dbg_last_source_mm(),
                      static_cast<std::int64_t>(kMax - initial) * static_cast<std::int64_t>(cells));
            EXPECT_TRUE(fixture.water().dbg_mass_ok());
        }
        chunk.water_src_mm.assign(cells, kMin);
        chunk.is_water_sleeping.store(false);
        fixture.tick();
        EXPECT_EQ(chunk.water_depth_mm, std::vector<std::int32_t>(cells, kMax));
        EXPECT_EQ(fixture.water().dbg_last_source_mm(), 0);
        for (const auto bed : {kMin, kMin + 1, -1, 0, kMax}) {
            chunk.water_bed_mm.assign(cells, bed);
            chunk.water_depth_mm.assign(cells, kMax);
            fixture.water().SetHydrology(true, 0, kMax);
            chunk.is_water_sleeping.store(false);
            fixture.tick();
            const auto expected = bed <= kMin + 1 ? kMax : (bed == -1 ? 1 : 0);
            EXPECT_EQ(chunk.water_depth_mm, std::vector<std::int32_t>(cells, expected));
            EXPECT_TRUE(fixture.water().dbg_mass_ok());
        }
    }
}

TEST(WaterSeedInputs, InternalAccumulationSaturatesOnceAndCountsExcessAsSink) {
    constexpr auto kMax = std::numeric_limits<std::int32_t>::max();
    for (const int resolution : {2, 8, 16}) {
        // These surface differences produce one mm on each of the corner's two incoming edges.
        const int bed_step = resolution == 2 ? 512 : (resolution == 8 ? 32 : 8);
        for (const int room : {2, 1}) {
            SeedFixture fixture;
            fixture.water().SetSimResolution(resolution);
            fixture.water().SetHydrology(true, 0, 0);
            fixture.tick();
            auto& chunk = *fixture.chunks().begin()->second;
            chunk.water_bed_mm.assign(chunk.water_bed_mm.size(), 1 + bed_step);
            chunk.water_bed_mm.back() = 1;
            chunk.water_depth_mm.assign(chunk.water_depth_mm.size(), kMax - room);
            chunk.water_edge_flux.assign(chunk.water_edge_flux.size(), 0);
            chunk.is_water_sleeping.store(false);
            fixture.tick();
            EXPECT_EQ(chunk.water_depth_mm.back(), kMax);
            EXPECT_EQ(fixture.water().dbg_last_sink_mm(), room == 2 ? 0 : 1);
            EXPECT_TRUE(fixture.water().dbg_mass_ok());
        }
        for (const auto momentum : {std::numeric_limits<std::int32_t>::min(), 0, kMax}) {
            SeedFixture fixture;
            fixture.water().SetSimResolution(resolution);
            fixture.water().SetHydrology(true, 0, 0);
            fixture.tick();
            auto& chunk = *fixture.chunks().begin()->second;
            chunk.water_bed_mm.assign(chunk.water_bed_mm.size(), kMax);
            chunk.water_depth_mm.assign(chunk.water_depth_mm.size(), kMax);
            chunk.water_edge_flux.assign(chunk.water_edge_flux.size(), momentum);
            chunk.water_bed_mm[0] = 0;
            chunk.is_water_sleeping.store(false);
            fixture.tick();
            EXPECT_EQ(chunk.water_depth_mm[0], 0); // sea sink follows the saturation sink
            EXPECT_GE(fixture.water().dbg_last_sink_mm(), kMax);
            EXPECT_TRUE(fixture.water().dbg_mass_ok());
            for (const auto depth : chunk.water_depth_mm)
                EXPECT_GE(depth, 0);
            for (const auto flux : chunk.water_edge_flux) {
                EXPECT_GE(flux, -2000000000);
                EXPECT_LE(flux, 2000000000);
            }
        }
    }
}

TEST(WaterSeedInputs, SeamTransferRespectsReceiverLimitInBothDirectionsAndMapOrders) {
    constexpr auto kMax = std::numeric_limits<std::int32_t>::max();
    for (const int resolution : {2, 8, 16}) {
        for (const bool reverse_flow : {false, true}) {
            for (const int room : {0, 1}) {
                SeedFixture a(2);
                SeedFixture b(2);
                for (auto* fixture : {&a, &b}) {
                    fixture->water().SetSimResolution(resolution);
                    fixture->water().SetHydrology(true, 0, 0);
                    fixture->tick();
                    for (const auto& [id, chunk] : fixture->chunks()) {
                        (void)id;
                        const bool donor = (chunk->get_coords().x == -4) != reverse_flow;
                        chunk->water_bed_mm.assign(chunk->water_bed_mm.size(), donor ? kMax : 1);
                        chunk->water_depth_mm.assign(chunk->water_depth_mm.size(),
                                                     donor ? kMax : kMax - room);
                        chunk->water_edge_flux.assign(chunk->water_edge_flux.size(), 0);
                        chunk->is_water_sleeping.store(false);
                    }
                }
                std::unordered_map<ChunkID, std::shared_ptr<Chunk>> reverse_chunks;
                std::vector<ChunkID> ids;
                for (const auto& [id, chunk] : b.chunks()) {
                    (void)chunk;
                    ids.push_back(id);
                }
                for (auto it = ids.rbegin(); it != ids.rend(); ++it)
                    reverse_chunks.emplace(*it, b.chunks().at(*it));
                entt::registry registry;
                a.tick();
                b.water().update(registry, reverse_chunks);
                ExpectSameWater(a, b);
                for (const auto& [id, chunk] : a.chunks()) {
                    (void)id;
                    const bool donor = (chunk->get_coords().x == -4) != reverse_flow;
                    const bool east = chunk->get_coords().x == -4;
                    for (int z = 0; z < resolution; ++z) {
                        const auto index =
                            static_cast<std::size_t>(z * resolution + (east ? resolution - 1 : 0));
                        EXPECT_EQ(chunk->water_depth_mm[index], donor ? kMax - room : kMax);
                    }
                }
                EXPECT_TRUE(a.water().dbg_mass_ok());
            }
        }
    }
}

TEST(WaterSeedInputs, BedEditsSaturateAtBothSignedLimits) {
    constexpr auto kMax = std::numeric_limits<std::int32_t>::max();
    constexpr auto kMin = std::numeric_limits<std::int32_t>::min();
    for (const int resolution : {2, 8, 16}) {
        SeedFixture fixture;
        fixture.water().SetSimResolution(resolution);
        fixture.tick();
        auto& chunk = *fixture.chunks().begin()->second;
        const auto cells = chunk.water_bed_mm.size();
        const Vec3 center = Vec3(chunk.get_coords() * IVec3(16, 0, 16)) + Vec3(8, 0, 8);
        for (const bool lower : {false, true}) {
            chunk.water_bed_mm.assign(cells, lower ? kMin + 1 : kMax - 1);
            for (int edit = 0; edit < 2; ++edit) {
                EXPECT_EQ(fixture.water().EditTerrainBed(center, lower ? -1 : 1, 16.0f),
                          resolution * resolution);
                EXPECT_EQ(chunk.water_bed_mm,
                          std::vector<std::int32_t>(cells, lower ? kMin : kMax));
            }
            // A full-range command must also widen before adding.
            fixture.water().EditTerrainBed(center, lower ? kMin : kMax, 16.0f);
            EXPECT_EQ(chunk.water_bed_mm, std::vector<std::int32_t>(cells, lower ? kMin : kMax));
        }
    }
}

TEST(WaterSeedInputs, ExternalSourcesAndDisplacementsBoundQuantizationAndMirrors) {
    constexpr auto kMax = std::numeric_limits<std::int32_t>::max();
    constexpr auto kMin = std::numeric_limits<std::int32_t>::min();
    SeedFixture fixture;
    fixture.water().SetSimResolution(8);
    fixture.water().SetHydrology(true, 0, 0);
    fixture.tick();
    auto& chunk = *fixture.chunks().begin()->second;
    const auto cells = chunk.water_depth_mm.size();
    const Vec3 base = Vec3(chunk.get_coords() * IVec3(16, 0, 16));
    const Vec3 center = base + Vec3(7, 0, 7);
    const auto center_index = std::size_t{3 * 8 + 3};
    chunk.water_bed_mm.assign(cells, kMax);
    // At the center, 0.1 m^3 / (4 m^2 * 25 cells) is exactly one rounded mm.
    for (const auto initial : {kMax - 1, kMax}) {
        chunk.water_depth_mm.assign(cells, initial);
        fixture.water().apply_displacement(center, 0.1f);
        EXPECT_EQ(chunk.water_depth_mm[center_index], kMax);
        EXPECT_FLOAT_EQ(chunk.water_level_data[center_index],
                        static_cast<float>(std::int64_t{2} * kMax) / 1000.0f);
    }
    for (const auto volume : {std::numeric_limits<float>::max(),
                              std::numeric_limits<float>::infinity(),
                              -std::numeric_limits<float>::max(),
                              -std::numeric_limits<float>::infinity(),
                              std::numeric_limits<float>::quiet_NaN()}) {
        chunk.water_depth_mm.assign(cells, kMax);
        fixture.water().apply_displacement(center, volume);
        const auto expected = volume < 0 ? 0 : kMax;
        EXPECT_EQ(chunk.water_depth_mm[center_index], expected);
    }
    for (const auto rate : {std::numeric_limits<float>::max(),
                            std::numeric_limits<float>::infinity(),
                            -std::numeric_limits<float>::infinity(),
                            std::numeric_limits<float>::quiet_NaN()}) {
        entt::registry registry;
        for (int z = 0; z < 8; ++z) {
            for (int x = 0; x < 8; ++x) {
                const auto entity = registry.create();
                registry.emplace<Components::TransformComponent>(entity).position =
                    base + Vec3(2 * x + 1, 0, 2 * z + 1);
                registry.emplace<Components::WaterSourceComponent>(entity).flow_rate = rate;
            }
        }
        chunk.water_depth_mm.assign(cells, kMax - 1);
        chunk.is_water_sleeping.store(false);
        const auto injected_before = fixture.water().debug_source_injected_mm();
        fixture.water().update(registry, fixture.chunks());
        const auto expected = rate > 0 ? kMax : kMax - 1;
        EXPECT_EQ(chunk.water_depth_mm, std::vector<std::int32_t>(cells, expected));
        EXPECT_EQ(fixture.water().debug_source_injected_mm() - injected_before,
                  rate > 0 ? static_cast<std::int64_t>(cells) : 0);
    }
    // Normalization and a negative displacement must not overflow on d + INT32_MIN.
    chunk.water_depth_mm.assign(cells, kMin);
    fixture.water().apply_displacement(center, -std::numeric_limits<float>::max());
    EXPECT_EQ(chunk.water_depth_mm[center_index], 0);
}

TEST(WaterSeedInputs, NegativeSavedDepthSourcesIgnoreEntityInsertionOrder) {
    constexpr auto kMin = std::numeric_limits<std::int32_t>::min();
    constexpr auto kMax = std::numeric_limits<std::int32_t>::max();
    SeedFixture first;
    SeedFixture second;
    const std::array<SeedFixture*, 2> fixtures{&first, &second};
    for (std::size_t order = 0; order < fixtures.size(); ++order) {
        auto& fixture = *fixtures[order];
        fixture.water().SetSimResolution(8);
        fixture.water().SetHydrology(true, 0, 0);
        fixture.tick();
        auto& chunk = *fixture.chunks().begin()->second;
        const auto cells = chunk.water_depth_mm.size();
        chunk.water_bed_mm.assign(cells, kMax);
        chunk.water_depth_mm.assign(cells, kMin);
        chunk.water_edge_flux.assign(2 * cells, 0);
        chunk.is_water_sleeping.store(false);
        const Vec3 base = Vec3(chunk.get_coords() * IVec3(16, 0, 16));
        entt::registry registry;
        // Two positive sources per cell must sum to three mm even on a negative saved depth.
        for (int z = 0; z < 8; ++z) {
            for (int x = 0; x < 8; ++x) {
                for (int source = 0; source < 2; ++source) {
                    const auto entity = registry.create();
                    registry.emplace<Components::TransformComponent>(entity).position =
                        base + Vec3(2 * x + 1, 0, 2 * z + 1);
                    const int millimetres = order == 0 ? source + 1 : 2 - source;
                    registry.emplace<Components::WaterSourceComponent>(entity).flow_rate =
                        static_cast<float>(millimetres) * 4.0f / (SECONDS_PER_TICK * 1000.0f);
                }
            }
        }
        fixture.water().update(registry, fixture.chunks());
        EXPECT_EQ(chunk.water_depth_mm, std::vector<std::int32_t>(cells, 3));
        EXPECT_EQ(fixture.water().debug_source_injected_mm(),
                  static_cast<std::int64_t>(cells) * (std::int64_t{3} - kMin));
    }
    ExpectSameWater(first, second);
}

TEST(WaterSeedInputs, SeedAndLegacyQuantizationBoundExtremeSurfaces) {
    constexpr auto kMax = std::numeric_limits<std::int32_t>::max();
    constexpr auto kMin = std::numeric_limits<std::int32_t>::min();
    for (const auto height : {2147483.0f,
                              2147484.0f,
                              -2147483.0f,
                              -2147484.0f,
                              std::numeric_limits<float>::max(),
                              -std::numeric_limits<float>::max(),
                              std::numeric_limits<float>::infinity(),
                              -std::numeric_limits<float>::infinity(),
                              std::numeric_limits<float>::quiet_NaN()}) {
        SeedFixture fixture;
        auto& chunk = *fixture.chunks().begin()->second;
        auto inputs = std::make_shared<HighWaterSeedInputs>();
        inputs->generation = {fixture.world().water_seed_terrain_generation(),
                              chunk.water_seed_edit_generation()};
        ComputeHighWaterSeedInputs(*inputs, chunk.get_coords(), fixture.world());
        inputs->terrain.fill(height);
        inputs->rest.fill(0.0f);
        chunk.stage_water_seed_inputs(std::move(inputs));
        fixture.tick();
        const auto bed = height > 2147483.0f    ? kMax
                         : height < -2147483.0f ? kMin
                         : std::isnan(height)
                             ? 0
                             : static_cast<std::int32_t>(std::round(height * 1000));
        EXPECT_EQ(chunk.water_bed_mm, std::vector<std::int32_t>(256, bed));
        for (const auto depth : chunk.water_depth_mm)
            EXPECT_GE(depth, 0);
        // Legacy seeding uses the same bounded conversion, independently of the bed.
        chunk.water_bed_mm.clear();
        chunk.water_depth_mm.clear();
        chunk.water_level_data.assign(256, height);
        chunk.water_flow_data.assign(256, Vec2(0));
        ASSERT_TRUE(fixture.water().ResizeSimulationGrid(chunk, Systems::WaterDetailLevel::Medium));
        for (const auto depth : chunk.water_depth_mm) {
            EXPECT_GE(depth, 0);
            if (std::isfinite(height) && height > 2147484.0f)
                EXPECT_EQ(depth, kMax);
            if (height < -2147484.0f || std::isnan(height))
                EXPECT_EQ(depth, 0);
        }
    }
}

TEST(WaterSeedInputs, MaximumWeatherRainSaturatesAtTheDepthLimit) {
    constexpr auto kMax = std::numeric_limits<std::int32_t>::max();
    SeedFixture fixture;
    fixture.water().SetSimResolution(2);
    fixture.tick();
    auto& chunk = *fixture.chunks().begin()->second;
    const Vec3 base = Vec3(chunk.get_coords() * IVec3(16, 0, 16));
    // This seed places two overlapping storms near the anchor by tick 270.
    Systems::WeatherSystem weather(719);
    bool saturated_rain = false;
    // A bounded schedule search uses only production weather, with no mutable test seam.
    for (std::uint64_t tick = 1; tick <= 512 && !saturated_rain; ++tick) {
        weather.Update(tick, base + Vec3(8, 0, 8), nullptr);
        for (int z = 0; z < 2; ++z)
            for (int x = 0; x < 2; ++x)
                saturated_rain |=
                    weather.PrecipitationAt(base + Vec3(8 * x + 4, 0, 8 * z + 4)) == 1.0f;
    }
    ASSERT_TRUE(saturated_rain);
    fixture.water().SetWeatherRain(&weather, kMax);
    fixture.water().SetHydrology(true, 0, 0);
    for (const auto initial : {kMax - 1, kMax}) {
        chunk.water_bed_mm.assign(4, kMax);
        chunk.water_depth_mm.assign(4, initial);
        chunk.water_edge_flux.assign(8, 0);
        chunk.is_water_sleeping.store(false);
        fixture.tick();
        EXPECT_TRUE(fixture.water().dbg_mass_ok());
        for (const auto depth : chunk.water_depth_mm)
            EXPECT_GE(depth, initial);
        EXPECT_LE(fixture.water().dbg_last_source_mm(), 4);
        if (initial == kMax - 1)
            EXPECT_GE(fixture.water().dbg_last_source_mm(), 1);
    }
}

TEST(WaterSeedInputs, WorldSurfaceProbesAndPersistenceKeepWideSurface) {
    constexpr auto kMax = std::numeric_limits<std::int32_t>::max();
    SeedFixture fixture;
    fixture.tick();
    auto& chunk = *fixture.chunks().begin()->second;
    chunk.water_bed_mm.assign(256, kMax);
    chunk.water_depth_mm.assign(256, kMax);
    chunk.water_edge_flux.assign(512, std::numeric_limits<std::int32_t>::min());
    chunk.sdf_data.resize(17u * 17u * 17u);
    const float surface = static_cast<float>(std::int64_t{2} * kMax) / 1000.0f;
    EXPECT_FLOAT_EQ(fixture.world().debug_deepest_water_pos(nullptr).y, surface);
    Vec3 water_pos(0);
    float land_x = 0, land_z = 0, water_surface = 0, bank_height = 0;
    EXPECT_FALSE(fixture.world().debug_find_shoreline(
        water_pos, land_x, land_z, water_surface, bank_height));
    chunk.water_bed_mm.assign(256, std::numeric_limits<std::int32_t>::min());
    WorldStreamingState saved;
    ASSERT_TRUE(saved.insert_chunk(fixture.chunks().begin()->second));
    WorldStreamingState restored;
    std::vector<std::string> errors;
    ASSERT_TRUE(Persistence::LoadWorldStreamingStateSnapshotJson(
        Persistence::SerializeWorldStreamingStateSnapshotJson(saved), restored, errors));
    const auto loaded = restored.snapshot_chunks();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded.front()->water_bed_mm, chunk.water_bed_mm);
    EXPECT_EQ(loaded.front()->water_depth_mm, chunk.water_depth_mm);
    EXPECT_EQ(loaded.front()->water_edge_flux, chunk.water_edge_flux);
    SHIELD_WorldSystem world(nullptr, nullptr, SeedParams(), 777);
    WaterSystem water(nullptr, &world);
    ASSERT_TRUE(world.adopt_streamed_chunk(loaded.front()));
    const std::unordered_map<ChunkID, std::shared_ptr<Chunk>> chunks{
        {loaded.front()->get_id(), loaded.front()}};
    water.SetHydrology(true, 0, 0);
    entt::registry registry;
    loaded.front()->is_water_sleeping.store(false);
    water.update(registry, chunks);
    EXPECT_EQ(loaded.front()->water_depth_mm, chunk.water_depth_mm);
    EXPECT_EQ(water.dbg_last_sink_mm(), 0);
    EXPECT_TRUE(water.dbg_mass_ok());
}

TEST(WaterSeedInputs, SavedIntegerCellsSaturateBeforeNarrowingAndRefuseNonintegers) {
    constexpr auto kMax = std::numeric_limits<std::int32_t>::max();
    constexpr auto kMin = std::numeric_limits<std::int32_t>::min();
    WorldStreamingState saved;
    ASSERT_TRUE(saved.get_or_create_chunk(IVec3(0)));
    const auto original =
        nlohmann::json::parse(Persistence::SerializeWorldStreamingStateSnapshotJson(saved));
    for (const auto* field : {"water_bed_mm", "water_depth_mm", "water_edge_flux"}) {
        auto snapshot = original;
        snapshot["chunks"][0][field] = {kMin,
                                        kMax,
                                        std::int64_t{kMin} - 1,
                                        std::int64_t{kMax} + 1,
                                        std::numeric_limits<std::int64_t>::min(),
                                        std::numeric_limits<std::uint64_t>::max()};
        WorldStreamingState loaded;
        std::vector<std::string> errors;
        EXPECT_TRUE(Persistence::ValidateWorldStreamingChunkFormatJson(snapshot["chunks"][0].dump(),
                                                                       errors));
        ASSERT_TRUE(
            Persistence::LoadWorldStreamingStateSnapshotJson(snapshot.dump(), loaded, errors));
        const auto restored = loaded.find_chunk(IVec3(0));
        if (!restored) {
            FAIL() << "Loaded water chunk is absent";
            return;
        }
        const auto& grid = std::string_view(field) == "water_bed_mm" ? restored->water_bed_mm
                           : std::string_view(field) == "water_depth_mm"
                               ? restored->water_depth_mm
                               : restored->water_edge_flux;
        EXPECT_EQ(grid, (std::vector<std::int32_t>{kMin, kMax, kMin, kMax, kMin, kMax}));
        for (const auto& invalid : {nlohmann::json{1e38},
                                    nlohmann::json{"bad"},
                                    nlohmann::json{nullptr},
                                    nlohmann::json{true},
                                    nlohmann::json(1),
                                    nlohmann::json(nullptr),
                                    nlohmann::json::object()}) {
            snapshot["chunks"][0][field] = invalid;
            errors.clear();
            EXPECT_FALSE(
                Persistence::LoadWorldStreamingStateSnapshotJson(snapshot.dump(), loaded, errors));
            EXPECT_TRUE(loaded.empty());
            EXPECT_FALSE(errors.empty());
            errors.clear();
            EXPECT_FALSE(Persistence::ValidateWorldStreamingChunkFormatJson(
                snapshot["chunks"][0].dump(), errors));
        }
        snapshot["chunks"][0][field] = nlohmann::json::array();
        errors.clear();
        EXPECT_TRUE(
            Persistence::LoadWorldStreamingStateSnapshotJson(snapshot.dump(), loaded, errors));
    }
    auto no_flux = original;
    no_flux["chunks"][0].erase("water_edge_flux");
    no_flux["chunks"][0]["water_depth_mm"] = {kMax, 0};
    WorldStreamingState loaded;
    std::vector<std::string> errors;
    ASSERT_TRUE(Persistence::LoadWorldStreamingStateSnapshotJson(no_flux.dump(), loaded, errors));
    const auto restored = loaded.find_chunk(IVec3(0));
    if (!restored) {
        FAIL() << "Loaded water chunk is absent";
        return;
    }
    EXPECT_EQ(restored->water_edge_flux, std::vector<std::int32_t>(4, 0));
}

TEST(WaterSeedInputs, NegativeLegacyDepthMigratesToZero) {
    for (const int old_res : {2, 8, 16}) {
        for (const int new_res : {2, 8, 16}) {
            if (old_res == new_res) {
                continue;
            }
            SCOPED_TRACE(::testing::Message() << old_res << " -> " << new_res);
            SeedFixture fixture;
            fixture.water().SetSimResolution(old_res);
            fixture.tick();
            auto& chunk = *fixture.chunks().begin()->second;
            const auto old_cells = static_cast<std::size_t>(old_res * old_res);
            ASSERT_EQ(chunk.water_bed_mm.size(), old_cells);
            ASSERT_EQ(chunk.water_depth_mm.size(), old_cells);
            chunk.water_bed_mm.assign(old_cells, -1000);
            chunk.water_depth_mm.assign(old_cells, std::numeric_limits<std::int32_t>::min());
            fixture.water().SetSimResolution(new_res);
            ASSERT_EQ(fixture.water().MigrateChunksToSimResolution(fixture.chunks()), 1u);
            const auto new_cells = static_cast<std::size_t>(new_res * new_res);
            EXPECT_EQ(chunk.water_bed_mm, std::vector<std::int32_t>(new_cells, -1000));
            EXPECT_EQ(chunk.water_depth_mm, std::vector<std::int32_t>(new_cells, 0));
            EXPECT_EQ(chunk.water_level_data, std::vector<float>(new_cells, -1.0f));
        }
    }
}

TEST(WaterSeedInputs, EditedChannelAndDamSurviveMigrationAndSavedContinuation) {
    // Every supported session resolution, both directions and identity controls.
    for (const int old_res : {2, 4, 8, 16}) {
        for (const int new_res : {2, 4, 8, 16}) {
            SCOPED_TRACE(::testing::Message() << old_res << " -> " << new_res);
            SeedFixture fixture;
            fixture.water().SetSimResolution(old_res);
            fixture.tick(); // also binds EditTerrainBed's active resident set
            auto& chunk = *fixture.chunks().begin()->second;
            const auto coords = chunk.get_coords();
            const float origin_x = static_cast<float>(coords.x * CHUNK_SIZE_X);
            const float origin_z = static_cast<float>(coords.z * CHUNK_SIZE_Z);
            const auto terrain = chunk.water_sim_terrain_height;
            const auto initial_beds = chunk.water_bed_mm;
            ASSERT_EQ(initial_beds.size(), static_cast<std::size_t>(old_res * old_res));
            ASSERT_GT(fixture.water().EditTerrainBed({origin_x, 0, origin_z + 8}, -4000, 8), 0);
            ASSERT_GT(fixture.water().EditTerrainBed({origin_x + 16, 0, origin_z + 8}, 4000, 8), 0);
            EXPECT_EQ(chunk.water_sim_terrain_height, terrain); // edits do not alter terrain
            const auto beds = chunk.water_bed_mm;
            const auto depths = chunk.water_depth_mm;
            const auto flux = chunk.water_edge_flux;
            ASSERT_LT(beds.front(), initial_beds.front());
            ASSERT_GT(beds.back(), initial_beds.back());
            // Persist the edited old grid, then migrate on load and compare
            // four real solver steps with the uninterrupted migrated control.
            WorldStreamingState saved;
            const auto saved_chunk = saved.get_or_create_chunk(coords);
            saved_chunk->has_water_sim.store(true);
            saved_chunk->current_water_resolution.store(old_res);
            saved_chunk->water_bed_mm = chunk.water_bed_mm;
            saved_chunk->water_depth_mm = chunk.water_depth_mm;
            saved_chunk->water_level_data = chunk.water_level_data;
            saved_chunk->water_flow_data = chunk.water_flow_data;
            saved_chunk->water_sim_terrain_height = chunk.water_sim_terrain_height;
            saved_chunk->water_edge_flux = chunk.water_edge_flux;
            saved_chunk->is_water_sleeping.store(chunk.is_water_sleeping.load());
            saved_chunk->max_water_delta_last_tick = chunk.max_water_delta_last_tick;
            saved_chunk->ticks_below_threshold = chunk.ticks_below_threshold;
            saved_chunk->water_mesh_dirty_ticks = chunk.water_mesh_dirty_ticks;
            fixture.water().SetSimResolution(new_res);
            EXPECT_EQ(fixture.water().MigrateChunksToSimResolution(fixture.chunks()),
                      old_res == new_res ? 0u : 1u);
            EXPECT_EQ(chunk.water_bed_mm, MigratedWaterOracle(beds, old_res, new_res));
            EXPECT_EQ(chunk.water_depth_mm, MigratedWaterOracle(depths, old_res, new_res));
            if (old_res == new_res) {
                EXPECT_EQ(chunk.water_edge_flux, flux);
            } else {
                for (std::size_t index = 0; index < chunk.water_bed_mm.size(); ++index) {
                    EXPECT_FLOAT_EQ(
                        chunk.water_level_data[index],
                        static_cast<float>(static_cast<std::int64_t>(chunk.water_bed_mm[index]) +
                                           chunk.water_depth_mm[index]) /
                            1000.0f);
                }
            }
            WorldStreamingState restored;
            std::vector<std::string> errors;
            ASSERT_TRUE(Persistence::LoadWorldStreamingStateSnapshotJson(
                Persistence::SerializeWorldStreamingStateSnapshotJson(saved), restored, errors));
            ASSERT_TRUE(errors.empty());
            const auto loaded = restored.snapshot_chunks();
            ASSERT_EQ(loaded.size(), 1u);
            ASSERT_TRUE(loaded.front());
            EXPECT_EQ(loaded.front()->water_bed_mm, beds);
            EXPECT_EQ(loaded.front()->water_depth_mm, depths);
            SHIELD_WorldSystem world(nullptr, nullptr, SeedParams(), 777);
            WaterSystem water(nullptr, &world);
            water.SetSimResolution(new_res);
            world.SetWaterSystem(&water);
            ASSERT_TRUE(world.adopt_streamed_chunk(loaded.front()));
            const std::unordered_map<ChunkID, std::shared_ptr<Chunk>> chunks{
                {loaded.front()->get_id(), loaded.front()}};
            EXPECT_EQ(water.MigrateChunksToSimResolution(chunks), old_res == new_res ? 0u : 1u);
            EXPECT_EQ(loaded.front()->water_bed_mm, chunk.water_bed_mm);
            EXPECT_EQ(loaded.front()->water_depth_mm, chunk.water_depth_mm);
            entt::basic_registry<> registry;
            for (int step = 0; step < 4; ++step) {
                water.update(registry, chunks);
                fixture.tick();
                EXPECT_EQ(loaded.front()->water_bed_mm, chunk.water_bed_mm);
                EXPECT_EQ(loaded.front()->water_depth_mm, chunk.water_depth_mm);
                EXPECT_EQ(loaded.front()->water_edge_flux, chunk.water_edge_flux);
            }
        }
    }
}

TEST(WaterSeedInputs,
     IntegerMigrationDoesNotRequireRenderMirrorsAndRefusesInvalidSourceResolution) {
    SeedFixture fixture;
    fixture.water().SetSimResolution(8);
    fixture.tick();
    auto& chunk = *fixture.chunks().begin()->second;
    for (std::size_t index = 0; index < chunk.water_bed_mm.size(); ++index) {
        chunk.water_bed_mm[index] = index % 2 == 0 ? std::numeric_limits<std::int32_t>::min()
                                                   : std::numeric_limits<std::int32_t>::max();
        chunk.water_depth_mm[index] = std::numeric_limits<std::int32_t>::max();
    }
    const auto beds = chunk.water_bed_mm;
    const auto depths = chunk.water_depth_mm;
    const int old_res = chunk.current_water_resolution.load();
    chunk.water_level_data.clear();
    chunk.water_flow_data.clear();
    fixture.water().SetSimResolution(16);
    EXPECT_EQ(fixture.water().MigrateChunksToSimResolution(fixture.chunks()), 1u);
    EXPECT_EQ(chunk.water_bed_mm, MigratedWaterOracle(beds, old_res, 16));
    EXPECT_EQ(chunk.water_depth_mm, MigratedWaterOracle(depths, old_res, 16));
    const auto complete_beds = chunk.water_bed_mm;
    const auto complete_depths = chunk.water_depth_mm;
    fixture.water().SetSimResolution(8);
    for (const bool partial_bed : {false, true}) {
        if (partial_bed)
            chunk.water_bed_mm.pop_back();
        else
            chunk.water_depth_mm.pop_back();
        const auto partial_beds = chunk.water_bed_mm;
        const auto partial_depths = chunk.water_depth_mm;
        EXPECT_EQ(fixture.water().MigrateChunksToSimResolution(fixture.chunks()), 0u);
        EXPECT_EQ(chunk.water_bed_mm, partial_beds);
        EXPECT_EQ(chunk.water_depth_mm, partial_depths);
        chunk.water_bed_mm = complete_beds;
        chunk.water_depth_mm = complete_depths;
    }
    for (const int invalid : {-1, 0, 1, 3, 33, std::numeric_limits<int>::max()}) {
        chunk.current_water_resolution.store(invalid);
        const auto before_beds = chunk.water_bed_mm;
        const auto before_depths = chunk.water_depth_mm;
        EXPECT_EQ(fixture.water().MigrateChunksToSimResolution(fixture.chunks()), 0u);
        EXPECT_EQ(chunk.current_water_resolution.load(), invalid);
        EXPECT_EQ(chunk.water_bed_mm, before_beds);
        EXPECT_EQ(chunk.water_depth_mm, before_depths);
    }
}

void ExpectRefusedWaterMirrors(const Chunk& chunk,
                               const std::vector<float>& terrain,
                               const std::vector<float>& rest,
                               const std::vector<Vec2>& flow) {
    EXPECT_EQ(chunk.water_sim_terrain_height, terrain);
    EXPECT_EQ(chunk.water_rest_level, rest);
    EXPECT_EQ(chunk.water_flow_data, flow);
    EXPECT_TRUE(chunk.water_mesh_generated.load());
    EXPECT_EQ(chunk.water_mesh_dirty_ticks, 9);
}

TEST(WaterSeedInputs, RejectedAdjacentGridsNeverEnterSolverSeamsOrExternalEdits) {
    for (const int target : {8, 16}) {
        for (const std::size_t partial : {0u, 1u, 255u, 257u}) {
            for (const bool partial_bed : {false, true}) {
                SeedFixture fixture(2);
                fixture.tick();
                fixture.water().SetSimResolution(target);
                fixture.water().SetHydrology(true, 3, 1);
                entt::registry sources;
                for (const auto& [id, chunk] : fixture.chunks()) {
                    (void)id;
                    auto& malformed = partial_bed ? chunk->water_bed_mm : chunk->water_depth_mm;
                    malformed.resize(partial, 700);
                    chunk->is_water_sleeping.store(false);
                    chunk->ticks_below_threshold = 7;
                    chunk->water_mesh_generated.store(true);
                    chunk->water_mesh_dirty_ticks = 9;
                    const auto entity = sources.create();
                    sources.emplace<Components::TransformComponent>(entity).position =
                        Vec3(chunk->get_coords() * IVec3(CHUNK_SIZE_X, 0, CHUNK_SIZE_Z)) +
                        Vec3(8.0f, 0.0f, 8.0f);
                    sources.emplace<Components::WaterSourceComponent>(entity).flow_rate = 1.0f;
                    EXPECT_FALSE(fixture.water().ResizeSimulationGrid(
                        *chunk, static_cast<Systems::WaterDetailLevel>(target)));
                }
                EXPECT_EQ(fixture.water().MigrateChunksToSimResolution(fixture.chunks()), 0u);
                std::unordered_map<ChunkID, std::vector<std::int32_t>> beds, depths, fluxes;
                std::unordered_map<ChunkID, std::vector<float>> levels, terrain, rest;
                std::unordered_map<ChunkID, std::vector<Vec2>> flows;
                for (const auto& [id, chunk] : fixture.chunks()) {
                    beds.emplace(id, chunk->water_bed_mm);
                    depths.emplace(id, chunk->water_depth_mm);
                    fluxes.emplace(id, chunk->water_edge_flux);
                    levels.emplace(id, chunk->water_level_data);
                    terrain.emplace(id, chunk->water_sim_terrain_height);
                    rest.emplace(id, chunk->water_rest_level);
                    flows.emplace(id, chunk->water_flow_data);
                }
                // Multiple ticks cover both rotating resize candidates and refusal retries.
                for (int tick = 0; tick < 3; ++tick) {
                    fixture.water().update(sources, fixture.chunks());
                    EXPECT_EQ(fixture.water().cells_stepped_last_update(), 0u);
                    EXPECT_EQ(fixture.water().dbg_awake_water_chunks(), 0u);
                    EXPECT_EQ(fixture.water().dbg_seam_wet_pairs(), 0);
                    for (const auto& [id, chunk] : fixture.chunks()) {
                        const Vec3 position =
                            Vec3(chunk->get_coords() * IVec3(CHUNK_SIZE_X, 0, CHUNK_SIZE_Z)) +
                            Vec3(8.0f, 0.0f, 8.0f);
                        EXPECT_FALSE(fixture.world().debug_water_grid_at(position.x, position.z));
                        fixture.water().apply_displacement(position, 1.0f);
                        EXPECT_EQ(fixture.water().EditTerrainBed(position, -100, 4.0f), 0);
                        EXPECT_EQ(chunk->current_water_resolution.load(), 16);
                        EXPECT_EQ(chunk->water_bed_mm, beds.at(id));
                        EXPECT_EQ(chunk->water_depth_mm, depths.at(id));
                        EXPECT_EQ(chunk->water_edge_flux, fluxes.at(id));
                        EXPECT_EQ(chunk->water_level_data, levels.at(id));
                        ExpectRefusedWaterMirrors(
                            *chunk, terrain.at(id), rest.at(id), flows.at(id));
                        EXPECT_FALSE(chunk->is_water_sleeping.load());
                        EXPECT_EQ(chunk->ticks_below_threshold, 7);
                    }
                }
            }
        }
    }
}

TEST(WaterSeedInputs, InvalidResolutionAndMissingIntegerArraysCannotWakeOrStep) {
    for (const int resolution : {0, 1, 3, 33, std::numeric_limits<int>::max()}) {
        SeedFixture fixture(2);
        fixture.tick();
        for (const auto& [id, chunk] : fixture.chunks()) {
            (void)id;
            chunk->current_water_resolution.store(resolution);
            chunk->is_water_sleeping.store(true);
        }
        fixture.water().SetHydrology(true, 3, 1);
        fixture.tick();
        EXPECT_EQ(fixture.water().cells_stepped_last_update(), 0u);
        for (const auto& [id, chunk] : fixture.chunks()) {
            (void)id;
            EXPECT_TRUE(chunk->is_water_sleeping.load());
        }
    }
    for (const int missing : {0, 1, 2}) {
        SeedFixture fixture(2);
        fixture.tick();
        auto& malformed = *fixture.chunks().begin()->second;
        if (missing == 0)
            malformed.water_bed_mm.clear();
        else if (missing == 1)
            malformed.water_depth_mm.clear();
        else {
            // Missing flux alone is valid: the solver initializes its momentum.
            // A missing pair member must still refuse even with missing flux.
            malformed.water_bed_mm.clear();
            malformed.water_edge_flux.clear();
        }
        malformed.is_water_sleeping.store(false);
        for (const auto& [id, chunk] : fixture.chunks()) {
            (void)id;
            if (chunk.get() != &malformed)
                chunk->is_water_sleeping.store(true);
        }
        fixture.tick();
        EXPECT_EQ(fixture.water().cells_stepped_last_update(), 0u);
        EXPECT_EQ(fixture.water().dbg_seam_wet_pairs(), 0);
        for (const auto& [id, chunk] : fixture.chunks()) {
            (void)id;
            if (chunk.get() != &malformed)
                EXPECT_TRUE(chunk->is_water_sleeping.load());
        }
    }
}

TEST(WaterSeedInputs, CompleteIntegerPairStepsWithAbsentOrWrongSizedFlux) {
    for (const std::size_t flux_size : {0u, 1u, 127u, 129u}) {
        SCOPED_TRACE(flux_size);
        SeedFixture adopted;
        SeedFixture reference;
        for (auto* fixture : {&adopted, &reference}) {
            fixture->water().SetSimResolution(8);
            fixture->water().SetHydrology(true, 3, 1);
            fixture->tick(); // Exercise normal chunk seeding before adoption.
            auto& chunk = *fixture->chunks().begin()->second;
            chunk.water_bed_mm.assign(64, 4000);
            chunk.water_depth_mm.assign(64, 2000);
            chunk.water_edge_flux.assign(128, 0);
            chunk.is_water_sleeping.store(false);
        }
        auto& chunk = *adopted.chunks().begin()->second;
        chunk.water_edge_flux.assign(flux_size, 700);
        const auto before = chunk.water_depth_mm;
        adopted.tick();
        reference.tick();
        EXPECT_EQ(adopted.water().cells_stepped_last_update(), 64u);
        EXPECT_NE(chunk.water_depth_mm, before);
        EXPECT_EQ(chunk.water_edge_flux.size(), 128u);
        ExpectSameWater(adopted, reference);
    }
}

TEST(WaterSeedInputs, CompletePairsWithoutFluxExchangeAcrossSeam) {
    SeedFixture fixture(2);
    fixture.water().SetSimResolution(8);
    fixture.water().SetHydrology(true, 0, 0);
    fixture.tick();
    for (const auto& [id, chunk] : fixture.chunks()) {
        (void)id;
        chunk->water_bed_mm.assign(64, 4000);
        chunk->water_depth_mm.assign(64, chunk->get_coords().x == -4 ? 2000 : 1000);
        chunk->water_edge_flux.clear();
        chunk->is_water_sleeping.store(false);
    }
    fixture.tick();
    EXPECT_EQ(fixture.water().cells_stepped_last_update(), 128u);
    EXPECT_GT(fixture.water().dbg_seam_wet_pairs(), 0);
    std::int64_t total_depth = 0;
    for (const auto& [id, chunk] : fixture.chunks()) {
        (void)id;
        EXPECT_EQ(chunk->water_edge_flux.size(), 128u);
        const auto original_depth = chunk->get_coords().x == -4 ? 2000 : 1000;
        EXPECT_NE(chunk->water_depth_mm, std::vector<std::int32_t>(64, original_depth));
        for (const auto depth : chunk->water_depth_mm)
            total_depth += depth;
    }
    EXPECT_EQ(total_depth, 64 * 3000);
}

TEST(WaterSeedInputs, SourceReadinessRequiresTheSameArraysAsInjection) {
    SeedFixture fixture;
    fixture.water().SetSimResolution(8);
    fixture.tick();
    auto& chunk = *fixture.chunks().begin()->second;
    const Vec3 position =
        Vec3(chunk.get_coords() * IVec3(CHUNK_SIZE_X, 0, CHUNK_SIZE_Z)) + Vec3(8.0f, 0.0f, 8.0f);
    const float terrain = fixture.world().GetTerrainHeightAt(position.x, position.z);
    const float expected_surface = terrain + 10.0f;
    ASSERT_NE(expected_surface, terrain); // Distinguish a live read from the terrain fallback.
    for (const int field : {0, 1, 2, 3, 4}) {
        for (const std::size_t length : {0u, 63u, 64u, 65u}) {
            SCOPED_TRACE(field);
            SCOPED_TRACE(length);
            chunk.water_bed_mm.assign(64, 4000);
            chunk.water_depth_mm.assign(64, 2000);
            chunk.water_level_data.assign(64, expected_surface);
            chunk.water_flow_data.assign(64, Vec2(0.0f));
            chunk.water_sim_terrain_height.assign(64, 4.0f);
            chunk.water_edge_flux.clear(); // Injection does not require momentum storage.
            if (field == 0)
                chunk.water_bed_mm.resize(length);
            else if (field == 1)
                chunk.water_depth_mm.resize(length);
            else if (field == 2)
                chunk.water_level_data.resize(length);
            else if (field == 3)
                chunk.water_flow_data.resize(length);
            else
                chunk.water_sim_terrain_height.resize(length);
            const bool ready = field < 2 ? length == 64u : length >= 64u;
            EXPECT_EQ(fixture.world().debug_water_grid_at(position.x, position.z), ready);
            if (ready)
                EXPECT_FLOAT_EQ(fixture.world().live_water_surface_at(position.x, position.z),
                                expected_surface);
        }
    }
    for (const int resolution : {0, 1, 3, 33, std::numeric_limits<int>::max()}) {
        chunk.current_water_resolution.store(resolution);
        EXPECT_FALSE(fixture.world().debug_water_grid_at(position.x, position.z));
    }
    chunk.current_water_resolution.store(8);
    chunk.has_water_sim.store(false);
    EXPECT_FALSE(fixture.world().debug_water_grid_at(position.x, position.z));
    EXPECT_FALSE(fixture.world().debug_water_grid_at(1000.0f, 1000.0f));
}

TEST(WaterSeedInputs, LegacyFloatPairMigratesTogetherAndThenSteps) {
    SeedFixture fixture;
    fixture.water().SetSimResolution(8);
    fixture.tick();
    auto& chunk = *fixture.chunks().begin()->second;
    chunk.water_bed_mm.clear();
    chunk.water_depth_mm.clear();
    chunk.water_edge_flux.clear();
    chunk.water_level_data.assign(64, 1000.0f);
    chunk.water_flow_data.assign(64, Vec2(0.0f));
    fixture.water().SetSimResolution(16);
    ASSERT_EQ(fixture.water().MigrateChunksToSimResolution(fixture.chunks()), 1u);
    ASSERT_EQ(chunk.water_bed_mm.size(), 256u);
    ASSERT_EQ(chunk.water_depth_mm.size(), 256u);
    EXPECT_EQ(chunk.water_edge_flux.size(), 512u);
    for (std::size_t index = 0; index < 256; ++index) {
        // Legacy floats quantize bed and depth independently at mm precision.
        EXPECT_NEAR(chunk.water_level_data[index], 1000.0f, 0.002f);
        EXPECT_GT(chunk.water_depth_mm[index], 0);
    }
    fixture.water().SetHydrology(true, 3, 1);
    chunk.is_water_sleeping.store(false);
    const auto before = chunk.water_depth_mm;
    fixture.tick();
    EXPECT_EQ(fixture.water().cells_stepped_last_update(), 256u);
    EXPECT_NE(chunk.water_depth_mm, before);
}

TEST(WaterSeedInputs, WorldWaterProbesRefuseUnsupportedResolutionBeforeSizeArithmetic) {
    SeedFixture fixture;
    fixture.tick();
    auto& chunk = *fixture.chunks().begin()->second;
    const auto coords = chunk.get_coords();
    const float x = static_cast<float>(coords.x * CHUNK_SIZE_X) + 8.0f;
    const float z = static_cast<float>(coords.z * CHUNK_SIZE_Z) + 8.0f;
    // A complete-looking 33x33 mirror is still outside the saved-grid contract.
    chunk.current_water_resolution.store(33);
    chunk.water_level_data.assign(33u * 33u, 10000.0f);
    EXPECT_FALSE(fixture.world().debug_water_grid_at(x, z));
    EXPECT_EQ(fixture.world().live_water_surface_at(x, z),
              fixture.world().GetTerrainHeightAt(x, z));
    // Extreme saved resolutions must refuse before res*res can overflow int.
    chunk.current_water_resolution.store(std::numeric_limits<int>::max());
    EXPECT_FALSE(fixture.world().debug_water_grid_at(x, z));
    EXPECT_EQ(fixture.world().live_water_surface_at(x, z),
              fixture.world().GetTerrainHeightAt(x, z));
}

TEST(WaterSeedInputs, NotReadyFallbackAndLateCompletionAreIdentical) {
    SeedFixture pending;
    SeedFixture ready;
    auto& pending_chunk = *pending.chunks().begin()->second;
    pending.prepare(pending_chunk, false);
    const auto late = pending_chunk.take_water_seed_inputs();
    pending_chunk.stage_water_seed_inputs(late);
    ready.prepare(*ready.chunks().begin()->second);
    const WaterAttribution::FrameScope attribution(true);
    pending.tick();
    ready.tick();
    EXPECT_EQ(attribution.snapshot().fallback_columns, 1u);
    EXPECT_EQ(attribution.snapshot().precomputed_columns, 1u);
    EXPECT_EQ(pending.water().dbg_seed_attribution().fallback_columns, 1u);
    EXPECT_EQ(ready.water().dbg_seed_attribution().precomputed_columns, 1u);
    EXPECT_FALSE(pending_chunk.water_seed_inputs());
    ExpectSameWater(pending, ready);
    ComputeHighWaterSeedInputs(*late, pending_chunk.get_coords(), pending.world());
    pending.tick();
    ready.tick();
    EXPECT_FALSE(pending_chunk.water_seed_inputs());
    EXPECT_EQ(pending.water().dbg_seed_attribution().fallback_columns, 0u);
    ExpectSameWater(pending, ready);
}

TEST(WaterSeedInputs, MixedReadinessPreservesHashedStateAndInitializationOrder) {
    SeedFixture prepared(13);
    SeedFixture fallback(13);
    SeedFixture mixed(13);
    std::vector<ChunkID> original_order;
    std::size_t index = 0;
    for (const auto& [id, chunk] : prepared.chunks()) {
        original_order.push_back(id);
        auto& other = *mixed.chunks().at(id);
        // Exercise the edited-terrain path in the same hash comparison.
        chunk->mark_voxel_data_dirty();
        fallback.chunks().at(id)->mark_voxel_data_dirty();
        other.mark_voxel_data_dirty();
        prepared.prepare(*chunk);
        mixed.prepare(other, index % 2 == 0);
        ++index;
    }
    for (std::size_t tick = 0; tick < 4; ++tick) {
        prepared.tick();
        fallback.tick();
        mixed.tick();
        const auto initialized = std::min((tick + 1) * 6, original_order.size());
        for (std::size_t i = 0; i < original_order.size(); ++i) {
            EXPECT_EQ(prepared.chunks().at(original_order[i])->has_water_sim.load(),
                      i < initialized);
        }
        EXPECT_EQ(prepared.world().debug_water_state_hash().water_chunks, initialized);
        ExpectSameWater(prepared, fallback);
        ExpectSameWater(prepared, mixed);
        if (tick < 2) {
            EXPECT_EQ(prepared.water().dbg_seed_attribution().precomputed_columns, 6u);
            EXPECT_EQ(fallback.water().dbg_seed_attribution().fallback_columns, 6u);
            EXPECT_EQ(mixed.water().dbg_seed_attribution().precomputed_columns, 3u);
            EXPECT_EQ(mixed.water().dbg_seed_attribution().fallback_columns, 3u);
        }
    }
    // Subsequent real water-bed edits also preserve the three hash trajectories.
    const Vec3 edit(-48.0f, 0.0f, -24.0f);
    EXPECT_GT(prepared.water().EditTerrainBed(edit, -4000, 8.0f), 0);
    fallback.water().EditTerrainBed(edit, -4000, 8.0f);
    mixed.water().EditTerrainBed(edit, -4000, 8.0f);
    prepared.tick();
    fallback.tick();
    mixed.tick();
    ExpectSameWater(prepared, fallback);
    ExpectSameWater(prepared, mixed);
}

TEST(WaterSeedInputs, ExistingGenerationWorkersPrepareBeforeActivation) {
    JobSystem jobs;
    jobs.startup(1);
    {
        SHIELD_WorldSystem world(&jobs, nullptr, SeedParams(), 777);
        WaterSystem water(&jobs, &world);
        world.SetWaterSystem(&water);
        const IVec3 coords(-4, 0, -2);
        world.dispatch_generation_jobs(std::vector<IVec3>{coords});
        world.wait_for_streaming_jobs();
        const auto chunk = world.find_streamed_chunk(coords);
        ASSERT_TRUE(chunk);
        ASSERT_TRUE(chunk->water_seed_inputs());
        ASSERT_TRUE(chunk->water_seed_inputs()->ready.load(std::memory_order_acquire));
        EXPECT_FALSE(chunk->has_water_sim.load());
        entt::basic_registry<> registry;
        const std::unordered_map<ChunkID, std::shared_ptr<Chunk>> chunks{{chunk->get_id(), chunk}};
        water.update(registry, chunks);
        EXPECT_TRUE(chunk->has_water_sim.load());
        EXPECT_EQ(water.dbg_seed_attribution().precomputed_columns, 1u);
        EXPECT_EQ(water.dbg_seed_attribution().fallback_columns, 0u);
        SeedFixture inline_path;
        inline_path.tick();
        const auto& inline_chunk = *inline_path.chunks().begin()->second;
        EXPECT_EQ(world.debug_water_state_hash().hash,
                  inline_path.world().debug_water_state_hash().hash);
        ASSERT_EQ(chunk->water_rest_level.size(), inline_chunk.water_rest_level.size());
        ASSERT_EQ(chunk->water_sim_terrain_height.size(),
                  inline_chunk.water_sim_terrain_height.size());
        for (std::size_t cell = 0; cell < chunk->water_rest_level.size(); ++cell) {
            EXPECT_EQ(std::bit_cast<std::uint32_t>(chunk->water_rest_level[cell]),
                      std::bit_cast<std::uint32_t>(inline_chunk.water_rest_level[cell]));
            EXPECT_EQ(std::bit_cast<std::uint32_t>(chunk->water_sim_terrain_height[cell]),
                      std::bit_cast<std::uint32_t>(inline_chunk.water_sim_terrain_height[cell]));
        }
    }
    jobs.shutdown();
}

TEST(WaterSeedInputs, BootSurfaceJobsPrepareHighInputsAndMediumKeepsExistingPath) {
    for (const int resolution : {8, 16}) {
        SHIELD_WorldSystem world(nullptr, nullptr, SeedParams(), 777);
        WaterSystem water(nullptr, &world);
        Systems::PhysicsSystem physics;
        water.SetSimResolution(resolution);
        world.SetWaterSystem(&water);
        ASSERT_TRUE(world.EnsureSurfaceReadyNear(Vec3(-48.0f, 0.0f, -32.0f), &physics, 0, 0));
        const auto chunks = world.snapshot_streamed_chunks();
        ASSERT_FALSE(chunks.empty());
        for (const auto& chunk : chunks) {
            ASSERT_TRUE(chunk);
            EXPECT_FALSE(chunk->has_water_sim.load());
            if (resolution == 16) {
                ASSERT_TRUE(chunk->water_seed_inputs());
                EXPECT_TRUE(chunk->water_seed_inputs()->ready.load(std::memory_order_acquire));
            } else {
                EXPECT_FALSE(chunk->water_seed_inputs());
            }
        }
    }
}

TEST(WaterSeedInputs, BootSurfaceAndGenerationJobsRespectPresetAndResolutionEligibility) {
    JobSystem jobs;
    jobs.startup(2);
    for (const bool threaded : {false, true}) {
        for (const bool lakes : {false, true}) {
            for (const int resolution : {2, 4, 8, 16}) {
                const bool eligible = lakes && resolution == 16;
                auto params = SeedParams();
                params.lakes_enabled = lakes;
                SHIELD_WorldSystem world(threaded ? &jobs : nullptr, nullptr, params, 777);
                WaterSystem water(threaded ? &jobs : nullptr, &world);
                Systems::PhysicsSystem physics;
                water.SetSimResolution(resolution);
                ASSERT_EQ(water.sim_resolution(), resolution);
                world.SetWaterSystem(&water);
                const IVec3 generated_coords(10, 0, 10);
                world.dispatch_generation_jobs(std::vector<IVec3>{generated_coords});
                world.wait_for_streaming_jobs();
                const auto generated = world.find_streamed_chunk(generated_coords);
                ASSERT_TRUE(generated);
                EXPECT_EQ(generated->water_seed_inputs() != nullptr, eligible);
                if (eligible) {
                    ASSERT_TRUE(generated->water_seed_inputs());
                    EXPECT_TRUE(generated->water_seed_inputs()->ready_for(resolution));
                }
                ASSERT_TRUE(
                    world.EnsureSurfaceReadyNear(Vec3(-48.0f, 0.0f, -32.0f), &physics, 0, 0));
                const auto chunks = world.snapshot_streamed_chunks();
                ASSERT_FALSE(chunks.empty());
                for (const auto& chunk : chunks) {
                    ASSERT_TRUE(chunk);
                    EXPECT_EQ(chunk->water_seed_inputs() != nullptr, eligible);
                    if (eligible) {
                        ASSERT_TRUE(chunk->water_seed_inputs());
                        EXPECT_TRUE(chunk->water_seed_inputs()->ready_for(resolution));
                    }
                }
            }
        }
    }
    jobs.shutdown();
}

TEST(WaterSeedInputs, AdoptionDropsInputsFromTheProducingWorld) {
    SeedFixture source;
    auto chunk = source.chunks().begin()->second;
    source.prepare(*chunk);
    SHIELD_WorldSystem destination(nullptr, nullptr, SeedParams(), 778);
    EXPECT_EQ(source.world().water_seed_terrain_generation(),
              destination.water_seed_terrain_generation());
    ASSERT_TRUE(destination.adopt_streamed_chunk(chunk));
    EXPECT_FALSE(chunk->water_seed_inputs());
}

// Occupy every real worker until the owner has queued a bounded input batch.
// Release is unconditional, including assertion failures, before joining jobs.
class BlockedSeedWorkers {
    struct Gate {
        std::mutex mutex;
        std::condition_variable changed;
        int entered = 0;
        bool release = false;
    };
    std::shared_ptr<Gate> m_gate = std::make_shared<Gate>();
    JobSystem m_jobs;

public:
    BlockedSeedWorkers() {
        m_jobs.startup(2);
        for (int i = 0; i < 2; ++i) {
            m_jobs.dispatch([gate = m_gate] {
                std::unique_lock lock(gate->mutex);
                ++gate->entered;
                gate->changed.notify_all();
                gate->changed.wait(lock, [&gate] { return gate->release; });
            });
        }
        std::unique_lock lock(m_gate->mutex);
        m_gate->changed.wait(lock, [this] { return m_gate->entered == 2; });
    }
    ~BlockedSeedWorkers() {
        release();
        m_jobs.shutdown();
    }
    JobSystem& jobs() {
        return m_jobs;
    }
    void release() {
        {
            std::lock_guard lock(m_gate->mutex);
            m_gate->release = true;
        }
        m_gate->changed.notify_all();
    }
};

TEST(WaterSeedInputs, BootMediumAndHighFallbackQueueBoundedInputsBeforeOwnerPublication) {
    for (const int resolution : {8, 16}) {
        for (const bool heightmap : {false, true}) {
            BlockedSeedWorkers workers;
            SeedFixture parallel(67, &workers.jobs());
            SeedFixture serial(67);
            parallel.water().SetSimResolution(resolution);
            serial.water().SetSimResolution(resolution);
            parallel.water().SetBootSettleMode(true);
            serial.water().SetBootSettleMode(true);
            for (const auto& [id, chunk] : parallel.chunks()) {
                if (heightmap) {
                    parallel.world().GenerateChunkData(*chunk, 2);
                    serial.world().GenerateChunkData(*serial.chunks().at(id), 2);
                    chunk->current_lod.store(0);
                    serial.chunks().at(id)->current_lod.store(0);
                }
                chunk->is_water_sleeping.store(true);
                serial.chunks().at(id)->is_water_sleeping.store(true);
            }
            std::atomic<bool> finished{false};
            std::thread owner([&] {
                parallel.tick();
                finished.store(true, std::memory_order_release);
            });
            std::size_t queued = 0;
            while (!finished.load(std::memory_order_acquire)) {
                queued = workers.jobs().get_runtime_stats().queue_depth;
                if (queued != 0)
                    break;
                std::this_thread::yield();
            }
            // A serial boot regression completes without ever submitting input jobs.
            EXPECT_EQ(queued, 64u);
            for (const auto& [id, chunk] : parallel.chunks()) {
                EXPECT_FALSE(chunk->has_water_sim.load());
                EXPECT_TRUE(chunk->water_bed_mm.empty());
                EXPECT_TRUE(chunk->water_rest_level.empty());
                EXPECT_TRUE(serial.chunks().at(id)->water_bed_mm.empty());
            }
            std::thread shutdown;
            if (resolution == 16 && !heightmap) {
                // Shutdown drains the queued batch; later batches are rejected.
                // Both outcomes must finish before owner publication can return.
                shutdown = std::thread([&workers] { workers.jobs().shutdown(); });
                while (workers.jobs().get_runtime_stats().accepting_jobs)
                    std::this_thread::yield();
            }
            workers.release();
            owner.join();
            if (shutdown.joinable())
                shutdown.join();
            serial.tick();
            ExpectSameWater(parallel, serial);
            EXPECT_EQ(parallel.world().debug_water_state_hash().water_chunks, 67u);
            EXPECT_EQ(parallel.water().cells_stepped_last_update(), 0u);
        }
    }
}

TEST(WaterSeedInputs, BootMixedReadyStaleAndLateInputsKeepSerialHashTrajectory) {
    JobSystem jobs;
    jobs.startup(2);
    {
        SeedFixture parallel(67, &jobs);
        SeedFixture serial(67);
        parallel.water().SetBootSettleMode(true);
        serial.water().SetBootSettleMode(true);
        std::vector<std::shared_ptr<HighWaterSeedInputs>> late;
        std::size_t index = 0;
        for (const auto& [id, chunk] : parallel.chunks()) {
            parallel.prepare(*chunk, index % 3 != 2);
            if (index % 3 == 1) {
                chunk->mark_voxel_data_dirty();
                serial.chunks().at(id)->mark_voxel_data_dirty();
            } else if (index % 3 == 2) {
                auto inputs = chunk->take_water_seed_inputs();
                late.push_back(inputs);
                chunk->stage_water_seed_inputs(std::move(inputs));
            }
            ++index;
        }
        for (int tick = 0; tick < 3; ++tick) {
            parallel.tick();
            serial.tick();
            ExpectSameWater(parallel, serial);
            if (tick == 0) {
                EXPECT_EQ(parallel.water().dbg_seed_attribution().precomputed_columns, 23u);
                EXPECT_EQ(parallel.water().dbg_seed_attribution().stale_columns, 22u);
                EXPECT_EQ(parallel.water().dbg_seed_attribution().fallback_columns, 44u);
                for (const auto& [id, chunk] : parallel.chunks()) {
                    EXPECT_FALSE(chunk->water_seed_inputs());
                    EXPECT_EQ(chunk->water_bed_mm, serial.chunks().at(id)->water_bed_mm);
                }
                // Late producers finish into retired storage, never into live grids.
                for (auto& inputs : late)
                    ComputeHighWaterSeedInputs(*inputs, IVec3(-4, 0, -2), parallel.world());
            }
        }
    }
    jobs.shutdown();
}

TEST(WaterSeedInputs, RejectedBootDispatchUsesOwnedInputsAndPausedBootDoesNothing) {
    JobSystem stopped_jobs;
    for (const int resolution : {2, 4, 8, 16}) {
        SeedFixture rejected(7, &stopped_jobs);
        SeedFixture serial(7);
        rejected.water().SetSimResolution(resolution);
        ASSERT_EQ(rejected.water().sim_resolution(), resolution);
        serial.water().SetSimResolution(resolution);
        ASSERT_EQ(serial.water().sim_resolution(), resolution);
        rejected.water().SetBootSettleMode(true);
        serial.water().SetBootSettleMode(true);
        rejected.water().SetBootPaused(true);
        rejected.tick();
        EXPECT_EQ(rejected.world().debug_water_state_hash().water_chunks, 0u);
        rejected.water().SetBootPaused(false);
        rejected.tick();
        serial.tick();
        ExpectSameWater(rejected, serial);
    }
}

TEST(WaterSeedInputs, DetachedSamplerSurvivesSourceResetPrefetchAndDestruction) {
    BlockedSeedWorkers workers;
    auto params = SeedParams();
    params.hydro_enabled = true;
    params.hydro_iterations = 1;
    params.rivers_enabled = true;
    params.island_mask_enabled = true;
    params.biomes_enabled = true;
    params.biome_relief_enabled = true;
    params.cliffs_enabled = true;
    params.surface_breaks_enabled = true;
    JobSystem prefetch_jobs;
    prefetch_jobs.startup(1);
    auto world = std::make_unique<SHIELD_WorldSystem>(&prefetch_jobs, nullptr, params, 777);
    // An in-flight prefetch must not become a callback on the detached sampler.
    world->PrefetchHydroRegions(-48.0f, -32.0f, 0.0f);
    auto sampler = world->capture_water_seed_sampler();
    std::weak_ptr<const SHIELD_WorldSystem> lifetime = sampler;
    HighWaterSeedInputs expected;
    {
        const auto scope = world->acquire_worldgen_sample_scope();
        ComputeHighWaterSeedInputs(expected, IVec3(-4, 0, -2), *world);
    }
    auto output = std::make_shared<HighWaterSeedInputs>();
    const auto handle = workers.jobs().dispatch_batch({[sampler, output] {
        ComputeHighWaterSeedInputs(*output, IVec3(-4, 0, -2), *sampler);
    }});
    sampler.reset();
    EXPECT_FALSE(lifetime.expired());
    EXPECT_FALSE(output->ready.load(std::memory_order_acquire));
    world->set_seed(778);
    params.height_offset += 5.0f;
    world->set_params(params);
    world.reset();
    prefetch_jobs.shutdown();
    EXPECT_FALSE(lifetime.expired());
    EXPECT_FALSE(output->ready.load(std::memory_order_acquire));
    workers.release();
    workers.jobs().wait(handle);
    workers.jobs().shutdown();
    EXPECT_TRUE(lifetime.expired());
    ASSERT_TRUE(output->ready.load(std::memory_order_acquire));
    EXPECT_EQ(output->terrain, expected.terrain);
    EXPECT_EQ(output->rest, expected.rest);
}

TEST(WaterSeedInputs, BoundedSamplerRefusesUnsupportedResolutionAndPartialTerrain) {
    SeedFixture fixture;
    for (const int resolution : {-1, 0, 1, 3, 17, 32, std::numeric_limits<int>::max()}) {
        HighWaterSeedInputs inputs;
        inputs.terrain.fill(123.0f);
        inputs.rest.fill(456.0f);
        ComputeWaterSeedInputs(inputs, IVec3(0), resolution, fixture.world());
        EXPECT_FALSE(inputs.ready.load(std::memory_order_acquire));
        EXPECT_EQ(inputs.resolution, 0);
        EXPECT_FALSE(inputs.ready_for(resolution));
        EXPECT_TRUE(std::all_of(inputs.terrain.begin(), inputs.terrain.end(), [](float value) {
            return value == 123.0f;
        }));
        EXPECT_TRUE(std::all_of(
            inputs.rest.begin(), inputs.rest.end(), [](float value) { return value == 456.0f; }));
    }
    HighWaterSeedInputs inputs;
    const std::array<float, 63> partial{};
    ComputeWaterSeedInputs(inputs, IVec3(0), 8, fixture.world(), partial);
    EXPECT_FALSE(inputs.ready.load(std::memory_order_acquire));
    EXPECT_EQ(inputs.terrain, (std::array<float, 256>{}));
    EXPECT_EQ(inputs.rest, (std::array<float, 256>{}));
    const std::array<float, 65> excess{};
    ComputeWaterSeedInputs(inputs, IVec3(0), 8, fixture.world(), excess);
    EXPECT_FALSE(inputs.ready.load(std::memory_order_acquire));
    EXPECT_EQ(inputs.resolution, 0);
    EXPECT_EQ(inputs.terrain, (std::array<float, 256>{}));
    EXPECT_EQ(inputs.rest, (std::array<float, 256>{}));
}

TEST(WaterSeedInputs, ReadinessRequiresPublishedSupportedAndMatchingResolution) {
    SeedFixture fixture;
    for (const int resolution : {2, 4, 8, 16}) {
        HighWaterSeedInputs inputs;
        inputs.resolution = resolution;
        EXPECT_FALSE(inputs.ready_for(resolution));
        const auto scope = fixture.world().acquire_worldgen_sample_scope();
        ComputeWaterSeedInputs(inputs, IVec3(0), resolution, fixture.world());
        ASSERT_TRUE(inputs.ready.load(std::memory_order_acquire));
        EXPECT_EQ(inputs.resolution, resolution);
        for (const int requested : {0, 2, 4, 8, 16, 32})
            EXPECT_EQ(inputs.ready_for(requested), requested == resolution);
    }
    HighWaterSeedInputs unknown;
    unknown.ready.store(true, std::memory_order_release);
    EXPECT_FALSE(unknown.ready_for(16));
    HighWaterSeedInputs oversized;
    oversized.resolution = 32;
    oversized.ready.store(true, std::memory_order_release);
    EXPECT_FALSE(oversized.ready_for(32));
}
} // namespace
