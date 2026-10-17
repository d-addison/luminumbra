#include <gtest/gtest.h>

#include "luminumbra/core/Types.h"

#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/IrrigationComponents.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/core/WaterComponents.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/systems/RegionalWater.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WaterSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/ResidentChunkRoster.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <latch>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {
using namespace Luminumbra;
using world::ActiveRegionLedger;
using world::RegionKey;
using world::RegionSchedule;
using world::RegionState;
using world::WorldClock;
constexpr std::uint64_t kCapacity = 2 * 64 + 4 * 8;

std::shared_ptr<Chunk> WaterChunk(const IVec3& coords, std::int32_t depth = 1000, int res = 8) {
    auto chunk = std::make_shared<Chunk>(coords);
    chunk->set_state(ChunkState::Idle);
    chunk->has_water_sim.store(true);
    chunk->current_water_resolution.store(res);
    const auto cells = static_cast<std::size_t>(res * res);
    chunk->water_depth_mm.assign(cells, depth);
    chunk->water_bed_mm.assign(cells, 1000);
    chunk->water_edge_flux.assign(2 * cells, 0);
    chunk->water_level_data.assign(cells, static_cast<float>(depth) / 1000.0f + 1.0f);
    chunk->water_flow_data.assign(cells, Vec2(0));
    chunk->water_sim_terrain_height.assign(cells, 1.0f);
    return chunk;
}

struct WaterState {
    std::vector<std::int32_t> depth, bed, flux, source;
    std::vector<float> level, terrain, rest;
    std::vector<Vec2> flow;
    bool initialized, sleeping, mesh, dirty, authority;
    int resolution, calm, mesh_ticks;
    float delta;
    std::uint32_t revision, voxel_revision;
    ChunkSdfProvenance provenance;
    bool operator==(const WaterState&) const = default;
};
WaterState CaptureWater(const Chunk& c) {
    return {c.water_depth_mm,
            c.water_bed_mm,
            c.water_edge_flux,
            c.water_src_mm,
            c.water_level_data,
            c.water_sim_terrain_height,
            c.water_rest_level,
            c.water_flow_data,
            c.has_water_sim.load(),
            c.is_water_sleeping.load(),
            c.water_mesh_generated.load(),
            c.is_water_state_dirty(),
            c.has_water_authority(),
            c.current_water_resolution.load(),
            c.ticks_below_threshold,
            c.water_mesh_dirty_ticks,
            c.max_water_delta_last_tick,
            c.water_revision(),
            c.voxel_revision(),
            c.sdf_provenance()};
}

class RegionalWaterFixture : public testing::Test {
protected:
    RegionalWaterFixture()
        : shield(nullptr, nullptr, Systems::TerrainGenParams{}, 1337)
        , water(nullptr, &shield) {
        water.SetSimResolution(static_cast<int>(Systems::WaterDetailLevel::Medium));
        water.SetHydrology(true, 0, 0);
    }
    void Owner(RegionKey key) {
        ledger.mark_edited(key, WorldClock{});
    }
    void Step(std::uint64_t windows, const std::vector<RegionKey>& due) {
        RegionSchedule schedule;
        schedule.tick = ledger.tick();
        schedule.due = due;
        water.UpdateRegional(registry, chunks, ledger, schedule, work, windows * kCapacity);
    }
    void Source(const Vec3& position, float rate = 1.0f) {
        const auto entity = registry.create();
        registry.emplace<Components::TransformComponent>(entity).position = position;
        registry.emplace<Components::WaterFlowSourceComponent>(entity).flow_rate = rate;
    }
    void CheckColdStagingTransactions();
    void CheckFlatStagingOracle();

    Systems::SHIELD_WorldSystem shield;
    Systems::WaterSystem water;
    ActiveRegionLedger ledger;
    entt::registry registry;
    std::vector<std::shared_ptr<Chunk>> chunks;
    std::vector<world::RegionWork> work;
};

// Occupy the sole worker until the host has published the next water tick.
// Shared gate ownership and release during unwinding keep assertion exits safe.
class MeshWorkerGate {
public:
    MeshWorkerGate(JobSystem& jobs, Systems::SHIELD_WorldSystem& shield)
        : m_shield(shield) {
        const auto state = m_state;
        jobs.dispatch([state]() {
            state->entered.count_down();
            state->released.wait();
        });
        state->entered.wait();
    }
    MeshWorkerGate(const MeshWorkerGate&) = delete;
    MeshWorkerGate& operator=(const MeshWorkerGate&) = delete;
    ~MeshWorkerGate() {
        Release();
        m_shield.quiesce_streaming_jobs_for_save();
    }
    void Release() {
        if (!m_released) {
            m_released = true;
            m_state->released.count_down();
        }
    }

private:
    struct State {
        std::latch entered{1};
        std::latch released{1};
    };
    Systems::SHIELD_WorldSystem& m_shield;
    std::shared_ptr<State> m_state = std::make_shared<State>();
    bool m_released = false;
};

void CheckDispatchedWaterSnapshot(bool active_mesh, int resolution) {
    SCOPED_TRACE(active_mesh);
    SCOPED_TRACE(resolution);
    JobSystem jobs;
    jobs.startup(1);
    Systems::SHIELD_WorldSystem shield(&jobs, nullptr, Systems::TerrainGenParams{}, 1337);
    Systems::WaterSystem water(nullptr, &shield);
    water.SetRegionalScheduling(true);
    water.SetSimResolution(8);
    water.SetHydrology(true, 100, 0);
    shield.SetWaterSystem(&water);
    auto chunk = WaterChunk({0, 0, 0}, 999000, resolution);
    chunk->sdf_data.assign(static_cast<std::size_t>(CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1) *
                               (CHUNK_SIZE_Z + 1),
                           -1.0f);
    if (active_mesh) {
        chunk->mesh_vertices = {{Vec3(0), Vec3(0, 1, 0), 0u},
                                {Vec3(1, 0, 0), Vec3(0, 1, 0), 0u},
                                {Vec3(0, 0, 1), Vec3(0, 1, 0), 0u}};
        chunk->mesh_indices = {0u, 1u, 2u};
        chunk->current_lod.store(0);
        chunk->set_state(ChunkState::Ready);
    }
    ASSERT_TRUE(shield.adopt_streamed_chunk(chunk));
    entt::registry registry;
    ActiveRegionLedger ledger;
    ledger.mark_edited({0, 0}, WorldClock{});
    RegionSchedule schedule;
    schedule.due = {{0, 0}};
    std::vector<world::RegionWork> work;
    const std::vector<std::shared_ptr<Chunk>> resident{chunk};
    const auto dispatched_levels = chunk->water_level_data;
    MeshWorkerGate gate(jobs, shield);
    shield.begin_tick(0);
    shield.update(registry, Vec3(8, 8, 8), nullptr);
    EXPECT_EQ(chunk->pending_lod.load(), 0);
    EXPECT_FALSE(chunk->pending_mesh_ready.load());

    // This is the real regional commit, including swapping all live mirrors.
    water.UpdateRegional(registry, resident, ledger, schedule, work, kCapacity);
    EXPECT_EQ(chunk->current_water_resolution.load(), 8);
    EXPECT_NE(chunk->water_level_data, dispatched_levels);
    const auto published_hash = shield.debug_water_state_hash();
    gate.Release();
    shield.quiesce_streaming_jobs_for_save();
    EXPECT_EQ(shield.debug_water_state_hash().hash, published_hash.hash);
    EXPECT_EQ(shield.debug_water_state_hash().water_chunks, published_hash.water_chunks);
    EXPECT_TRUE(chunk->pending_mesh_ready.load());
    EXPECT_FALSE(chunk->pending_mesh_failed.load());
    EXPECT_FALSE(chunk->water_mesh_generated.load());
    EXPECT_EQ(chunk->pending_water_mesh_vertices.size(),
              static_cast<std::size_t>(resolution) * resolution * 4u);
    EXPECT_EQ(chunk->pending_water_mesh_indices.size(),
              static_cast<std::size_t>(resolution) * resolution * 6u);
    for (const auto& vertex : chunk->pending_water_mesh_vertices)
        EXPECT_EQ(vertex.position.y, 1000.0f);
    shield.wait_for_streaming_jobs();
    EXPECT_EQ(shield.debug_water_state_hash().hash, published_hash.hash);
    EXPECT_EQ(shield.debug_water_state_hash().water_chunks, published_hash.water_chunks);
    // Publishing the old render snapshot must leave the newer water due for remeshing.
    EXPECT_FALSE(chunk->water_mesh_generated.load());
    shield.update(registry, Vec3(8, 8, 8), nullptr);
    shield.quiesce_streaming_jobs_for_save();
    EXPECT_TRUE(chunk->pending_mesh_ready.load());
    EXPECT_FALSE(chunk->water_mesh_generated.load());
    shield.wait_for_streaming_jobs();
    EXPECT_TRUE(chunk->water_mesh_generated.load());
    EXPECT_EQ(shield.debug_water_state_hash().hash, published_hash.hash);
}

TEST(RegionalWater, DispatchedMeshRetainsWaterAcrossPublicationAndResize) {
    // Idle hole fills use the High lane; Ready water remeshes use Normal.
    for (const bool active_mesh : {false, true})
        for (const int resolution : {4, 8})
            ASSERT_NO_FATAL_FAILURE(CheckDispatchedWaterSnapshot(active_mesh, resolution));
}

TEST(RegionalWater, MeshSnapshotRefusesMalformedSizesAndOutOfBoundResolutions) {
    for (const int fault : {0, 1, 2}) {
        SCOPED_TRACE(fault);
        Systems::SHIELD_WorldSystem shield(nullptr, nullptr, Systems::TerrainGenParams{}, 1337);
        auto valid = WaterChunk({0, 0, 0});
        auto chunk = WaterChunk({1, 0, 0});
        chunk->sdf_data.assign(static_cast<std::size_t>(CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1) *
                                   (CHUNK_SIZE_Z + 1),
                               -1.0f);
        valid->sdf_data = chunk->sdf_data;
        ASSERT_TRUE(shield.adopt_streamed_chunk(valid));
        if (fault == 0)
            chunk->water_flow_data.pop_back();
        else
            chunk->current_water_resolution.store(fault == 1 ? -1 : 33);
        ASSERT_TRUE(shield.adopt_streamed_chunk(chunk));
        entt::registry registry;
        EXPECT_THROW(shield.update(registry, Vec3(8, 8, 8), nullptr), std::runtime_error);
        EXPECT_EQ(valid->pending_lod.load(), -1);
        EXPECT_EQ(chunk->pending_lod.load(), -1);
        EXPECT_FALSE(chunk->pending_mesh_ready.load());
    }
}

// Rebuild the old admitted map, neighbors and face tuples without using cache indexes.
// Compare physical orientation as well as order: sorting must never exchange endpoints.
void CheckPlanningOracle(Systems::RegionalWater::PlanningCache& cache,
                         const std::vector<std::shared_ptr<Chunk>>& resident,
                         const ActiveRegionLedger& ledger,
                         int resolution,
                         bool raining,
                         const std::set<ChunkID>& source_targets) {
    using FaceKey = std::tuple<RegionKey, RegionKey, ChunkID, ChunkID, bool>;
    std::map<ChunkID, std::shared_ptr<Chunk>> rebuilt;
    const auto owner_of = [](const Chunk& chunk) {
        const auto coords = chunk.get_coords();
        return ActiveRegionLedger::region_at(Vec3(static_cast<float>(coords.x * CHUNK_SIZE_X),
                                                  0.0f,
                                                  static_cast<float>(coords.z * CHUNK_SIZE_Z)));
    };
    for (const auto& chunk : resident) {
        if (!chunk || chunk->get_state() == ChunkState::Loading)
            continue;
        const auto found = ledger.records().find(owner_of(*chunk));
        if (found != ledger.records().end() && found->second.state != RegionState::Frozen) {
            ASSERT_TRUE(rebuilt.emplace(chunk->get_id(), chunk).second);
        }
    }
    std::vector<ChunkID> expected_ids, actual_ids;
    for (const auto& [id, chunk] : rebuilt)
        expected_ids.push_back(id);
    for (const auto& entry : cache.entries) {
        if (entry.admitted) {
            actual_ids.push_back(entry.id);
            ASSERT_EQ(entry.chunk, rebuilt.at(entry.id).get());
        }
    }
    ASSERT_EQ(actual_ids, expected_ids);
    const auto neighbor_of = [&rebuilt](const Chunk& chunk, IVec3 offset) {
        const auto coords = chunk.get_coords() + offset;
        if (coords.x < -1048576 || coords.x >= 1048576 || coords.z < -1048576 ||
            coords.z >= 1048576)
            return rebuilt.end();
        return rebuilt.find(Chunk::calculate_id(coords));
    };
    constexpr IVec3 offsets[] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
    std::vector<FaceKey> expected_faces, actual_faces;
    for (const auto& [id, chunk] : rebuilt) {
        const auto* entry = cache.Find(id);
        ASSERT_NE(entry, nullptr);
        for (std::size_t direction = 0; direction < 4; ++direction) {
            const auto neighbor = neighbor_of(*chunk, offsets[direction]);
            const auto index = entry->neighbors[direction];
            const auto* cached =
                index == Systems::RegionalWater::missing ? nullptr : &cache.entries.at(index);
            if (neighbor == rebuilt.end()) {
                EXPECT_TRUE(!cached || !cached->admitted);
            } else {
                ASSERT_NE(cached, nullptr);
                ASSERT_TRUE(cached->admitted);
                EXPECT_EQ(cached->id, neighbor->first);
                if (direction == 0 || direction == 2)
                    expected_faces.emplace_back(owner_of(*chunk),
                                                owner_of(*neighbor->second),
                                                id,
                                                neighbor->first,
                                                direction == 0);
            }
        }
    }
    std::sort(expected_faces.begin(), expected_faces.end());
    for (const auto& indexed : cache.faces) {
        if (!cache.entries.at(indexed.positive).admitted ||
            !cache.entries.at(indexed.other).admitted)
            continue;
        const auto& face = indexed.face;
        EXPECT_EQ(cache.entries[indexed.positive].id, face.positive);
        EXPECT_EQ(cache.entries[indexed.other].id, face.other);
        actual_faces.emplace_back(face.owner, face.neighbor, face.positive, face.other, face.is_x);
    }
    EXPECT_EQ(actual_faces, expected_faces);

    RegionSchedule schedule;
    std::map<RegionKey, std::vector<ChunkID>> expected_candidates;
    for (const auto& [owner, record] : ledger.records()) {
        if (record.state != RegionState::Frozen) {
            schedule.due.push_back(owner);
            expected_candidates[owner]; // Retain claims for due owners without residents.
        }
    }
    std::reverse(schedule.due.begin(), schedule.due.end());
    for (const auto& [id, chunk] : rebuilt) {
        bool eligible = !chunk->has_water_sim.load() ||
                        chunk->current_water_resolution.load() != resolution ||
                        !chunk->is_water_sleeping.load() || raining || source_targets.contains(id);
        for (const auto offset : offsets) {
            const auto neighbor = neighbor_of(*chunk, offset);
            if (neighbor != rebuilt.end() && neighbor->second->has_water_sim.load() &&
                !neighbor->second->is_water_sleeping.load())
                eligible = true;
        }
        if (eligible)
            expected_candidates.at(owner_of(*chunk)).push_back(id);
    }
    for (const auto id : source_targets) {
        if (auto* entry = cache.Find(id))
            entry->source = true;
    }
    cache.PlanCandidates(ledger, schedule, raining);
    ASSERT_EQ(cache.claims.size(), expected_candidates.size());
    std::size_t claim = 0;
    for (const auto& [owner, ids] : expected_candidates) {
        EXPECT_EQ(cache.claims[claim].key, owner);
        EXPECT_EQ(cache.claims[claim++].awake, ids.size());
        std::vector<ChunkID> actual;
        if (const auto* group = cache.FindCandidates(owner)) {
            for (const auto index : group->ids)
                actual.push_back(cache.entries.at(index).id);
        }
        EXPECT_EQ(actual, ids);
    }
}

void CheckIncidentSelection(Systems::RegionalWater::PlanningCache& cache) {
    ASSERT_LE(cache.entries.size(), 12u);
    const auto subsets = std::size_t{1} << cache.entries.size();
    for (std::size_t mask = 0; mask < subsets; ++mask) {
        cache.selected.clear();
        cache.reserved.clear();
        for (std::size_t i = 0; i < cache.entries.size(); ++i) {
            auto& entry = cache.entries[i];
            entry.selected = entry.admitted && (mask & (std::size_t{1} << i)) != 0;
            if (entry.selected)
                cache.selected.push_back(i);
        }
        std::vector<std::size_t> expected;
        for (std::size_t i = 0; i < cache.faces.size(); ++i) {
            const auto& face = cache.faces[i];
            const bool selected =
                cache.entries[face.positive].selected && cache.entries[face.other].selected;
            if (face.face.owner != face.face.neighbor && selected)
                cache.reserved.push_back(i);
            if (selected)
                expected.push_back(i);
        }
        // Endpoint visitation and reservation order must not affect Face ordering.
        std::reverse(cache.selected.begin(), cache.selected.end());
        std::reverse(cache.reserved.begin(), cache.reserved.end());
        cache.SelectExecutedFaces();
        EXPECT_EQ(cache.executed, expected);
        for (const auto index : cache.executed) {
            const auto& face = cache.faces.at(index);
            EXPECT_EQ(cache.entries.at(face.positive).id, face.face.positive);
            EXPECT_EQ(cache.entries.at(face.other).id, face.face.other);
        }
    }
}

TEST_F(RegionalWaterFixture, CachedTopologyFacesAndCandidatesMatchScratchAcrossAdmissionAndChurn) {
    Owner({0, 0});
    Owner({1, 0});
    Owner({-1, -1});
    Owner({0, -1});
    Owner({32767, 0});
    Owner({-32768, 0});
    Owner({9, 9}); // Due owner with no resident candidates.
    auto a = WaterChunk({31, 0, 0}), b = WaterChunk({32, 0, 0});
    chunks = {a,
              b,
              WaterChunk({31, 0, 1}),
              WaterChunk({32, 0, 1}),
              WaterChunk({-1, 0, -1}),
              WaterChunk({0, 0, -1}),
              WaterChunk({1048575, 0, 0}),
              WaterChunk({-1048576, 0, 0}),
              WaterChunk({64, 0, 0}), // Unknown owner until admitted below.
              nullptr};
    Systems::RegionalWater::PlanningCache cache;
    for (int step = 0; step < 14; ++step) {
        SCOPED_TRACE(step);
        auto& records =
            const_cast<std::map<RegionKey, world::ActiveRegionRecord>&>(ledger.records());
        if (step == 1)
            std::reverse(chunks.begin(), chunks.end());
        if (step == 2)
            std::erase(chunks, b); // Park, retaining durable identity outside residency.
        if (step == 3)
            chunks.push_back(b); // Unpark the same object.
        if (step == 4) {
            const auto found = std::find(chunks.begin(), chunks.end(), b);
            b = WaterChunk({32, 0, 0}, 333); // Same ID, replacement live object.
            *found = b;
        }
        if (step == 5) {
            const auto found = std::find(chunks.begin(), chunks.end(), b);
            b = WaterChunk({33, 0, 0}, 111); // Changed packed identity/face membership.
            *found = b;
        }
        if (step == 6)
            a->set_state(ChunkState::Loading);
        if (step == 7)
            a->set_state(ChunkState::Idle);
        if (step == 8)
            records.at({1, 0}).state = RegionState::Frozen;
        if (step == 9)
            records.at({1, 0}).state = RegionState::Reduced;
        if (step == 10)
            Owner({2, 0});
        for (const auto& chunk : chunks) {
            if (chunk)
                chunk->is_water_sleeping.store(step % 3 != 0);
        }
        a->is_water_sleeping.store(false); // One-hop wake next to sleepers.
        if (step == 13) {
            // Admission/preparation flags must be fresh even with unchanged topology.
            a->has_water_sim.store(false);
            a->water_depth_mm.clear();
            a->water_bed_mm.clear();
            a->water_edge_flux.clear();
            a->water_level_data.clear();
            a->water_flow_data.clear();
            a->water_sim_terrain_height.clear();
        }
        const auto* entries_before = cache.entries.data();
        const auto* faces_before = cache.faces.data();
        const int resolution = step == 11 ? 16 : 8;
        cache.Refresh(chunks, ledger, resolution);
        ASSERT_NO_FATAL_FAILURE(
            CheckPlanningOracle(cache, chunks, ledger, resolution, step == 12, {b->get_id()}));
        ASSERT_NO_FATAL_FAILURE(CheckIncidentSelection(cache));
        if (step == 1 || step >= 8) {
            // A permutation, freeze/thaw, new owner, config or flag refresh does
            // not sort/reallocate geometry; filtering retains scratch face order.
            EXPECT_EQ(cache.entries.data(), entries_before);
            EXPECT_EQ(cache.faces.data(), faces_before);
        }
    }
    const auto duplicate = chunks;
    chunks.push_back(b);
    EXPECT_THROW(cache.Refresh(chunks, ledger, 8), std::runtime_error);
    chunks = duplicate;
    cache.Refresh(chunks, ledger, 8);
    ASSERT_NO_FATAL_FAILURE(CheckPlanningOracle(cache, chunks, ledger, 8, false, {}));
    ASSERT_NO_FATAL_FAILURE(CheckIncidentSelection(cache));
}

TEST_F(RegionalWaterFixture, WarmPlanningCacheStillValidatesUnselectedWaterAndRefusesAtomically) {
    Owner({0, 0});
    ledger.pin({0, 0}, true, WorldClock{});
    auto a = WaterChunk({0, 0, 0}), b = WaterChunk({4, 0, 0});
    chunks = {a, b};
    Step(2, {{0, 0}}); // Populate topology and transaction buffers first.
    for (int fault = 0; fault < 9; ++fault) {
        SCOPED_TRACE(fault);
        WorldClock clock;
        clock.set_tick(static_cast<std::uint64_t>(fault + 1));
        auto schedule = ledger.schedule(clock, std::span<const Vec3>{});
        schedule.due = {{0, 0}};
        b->is_water_sleeping.store(true); // Off-window state still requires validation.
        const auto valid = CaptureWater(*b);
        const float infinity = std::numeric_limits<float>::infinity();
        switch (fault) {
            case 0:
                b->water_depth_mm[0] = -1;
                break;
            case 1:
                b->water_level_data[0] = infinity;
                break;
            case 2:
                b->water_flow_data[0].y = infinity;
                break;
            case 3:
                b->water_sim_terrain_height[0] = infinity;
                break;
            case 4:
                b->ticks_below_threshold = -1;
                break;
            case 5:
                b->water_mesh_dirty_ticks = 16;
                break;
            case 6:
                b->max_water_delta_last_tick = infinity;
                break;
            case 7:
                b->current_water_resolution.store(3);
                break;
            case 8:
                b->water_flow_data.pop_back();
                break;
        }
        const auto before_a = CaptureWater(*a), before_b = CaptureWater(*b);
        const auto encoded = ledger.encode();
        const auto telemetry = water.regional_work();
        const auto revision = shield.far_lod_authority_revision();
        const auto completed = work;
        EXPECT_THROW(water.UpdateRegional(registry, chunks, ledger, schedule, work, kCapacity),
                     std::runtime_error);
        EXPECT_EQ(CaptureWater(*a), before_a);
        EXPECT_EQ(CaptureWater(*b), before_b);
        EXPECT_EQ(ledger.encode(), encoded);
        EXPECT_EQ(water.regional_work(), telemetry);
        EXPECT_EQ(shield.far_lod_authority_revision(), revision);
        ASSERT_EQ(work.size(), completed.size());
        for (std::size_t i = 0; i < work.size(); ++i) {
            EXPECT_EQ(work[i].key, completed[i].key);
            EXPECT_EQ(work[i].region_ticks, completed[i].region_ticks);
            EXPECT_EQ(work[i].entity_updates, completed[i].entity_updates);
            EXPECT_EQ(work[i].water_cells, completed[i].water_cells);
            EXPECT_EQ(work[i].page_updates, completed[i].page_updates);
        }
        b->water_depth_mm = valid.depth;
        b->water_level_data = valid.level;
        b->water_flow_data = valid.flow;
        b->water_sim_terrain_height = valid.terrain;
        b->ticks_below_threshold = valid.calm;
        b->water_mesh_dirty_ticks = valid.mesh_ticks;
        b->max_water_delta_last_tick = valid.delta;
        b->current_water_resolution.store(valid.resolution);
        // Retry the refused tick with the same identities; no poisoned derived flags.
        EXPECT_NO_THROW(water.UpdateRegional(registry, chunks, ledger, schedule, work, kCapacity));
    }
}

TEST_F(RegionalWaterFixture, SameRosterLateRefusalRetryMatchesFreshCacheCompleteOutputs) {
    Owner({0, 0});
    Owner({1, 0});
    auto a = WaterChunk({31, 0, 0}), b = WaterChunk({32, 0, 0});
    chunks = {a, b};
    const auto roster = chunks; // Keep both object identities alive through refusal and retry.
    std::vector<std::shared_ptr<Chunk>> reference{WaterChunk(a->get_coords()),
                                                  WaterChunk(b->get_coords())};
    Systems::SHIELD_WorldSystem reference_shield(
        nullptr, nullptr, Systems::TerrainGenParams{}, 1337);
    auto reference_ledger = ledger;
    std::vector<world::RegionWork> reference_work;
    {
        // Warm both worlds identically; only the actual system retains its cache.
        Systems::WaterSystem warm_reference(nullptr, &reference_shield);
        warm_reference.SetSimResolution(static_cast<int>(Systems::WaterDetailLevel::Medium));
        water.SetHydrology(true, 1, 0);
        warm_reference.SetHydrology(true, 1, 0);
        RegionSchedule warm_schedule;
        warm_schedule.tick = ledger.tick();
        warm_schedule.due = {{0, 0}, {1, 0}};
        water.UpdateRegional(registry, chunks, ledger, warm_schedule, work, 4 * kCapacity);
        warm_reference.UpdateRegional(
            registry, reference, reference_ledger, warm_schedule, reference_work, 4 * kCapacity);
    }
    // Preserve non-water contributions too, checking every completed-work field.
    const world::RegionWork prior_work{{0, 0}, 2, 3, 5, 7};
    work.push_back(prior_work);
    reference_work.push_back(prior_work);
    const auto capture_work = [](const auto& completed) {
        std::vector<
            std::tuple<RegionKey, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t>>
            state;
        for (const auto& entry : completed)
            state.emplace_back(entry.key,
                               entry.region_ticks,
                               entry.entity_updates,
                               entry.water_cells,
                               entry.page_updates);
        return state;
    };
    const auto capture_telemetry = [](const Systems::WaterSystem& system) {
        return std::make_tuple(system.regional_work(),
                               system.debug_sources_seen(),
                               system.debug_source_injected_mm(),
                               system.dbg_last_source_mm(),
                               system.dbg_last_sink_mm(),
                               system.dbg_mass_ok(),
                               system.dbg_seam_wet_pairs(),
                               system.dbg_awake_water_chunks(),
                               system.dbg_cells_simmed(),
                               system.cells_stepped_last_update());
    };
    const auto capture_authority = [](const Systems::SHIELD_WorldSystem& system) {
        return std::make_tuple(system.far_lod_authority_revision(),
                               system.far_lod_region_authority_revision(0, 0),
                               system.far_lod_region_authority_revision(1, 0));
    };
    const auto save_bytes = [](const auto& residents) {
        WorldStreamingState saved;
        for (const auto& chunk : residents)
            EXPECT_TRUE(saved.insert_chunk(chunk));
        return Persistence::SerializeWorldStreamingStateSnapshotJson(saved);
    };

    water.SetHydrology(true, 0, 0);
    Source({497, 0, 1});
    auto schedule = ledger.schedule(WorldClock(1), std::span<const Vec3>{});
    schedule.due = {{0, 0}, {1, 0}};
    reference_ledger = ledger;
    const auto valid_a = CaptureWater(*a), valid_b = CaptureWater(*b);
    ASSERT_EQ(valid_a, CaptureWater(*reference[0]));
    ASSERT_EQ(valid_b, CaptureWater(*reference[1]));
    ASSERT_EQ(save_bytes(chunks), save_bytes(reference));
    ASSERT_EQ(capture_work(work), capture_work(reference_work));
    ASSERT_EQ(capture_authority(shield), capture_authority(reference_shield));

    // Both interior kernels and the ECS injection succeed, mutating reused payloads.
    // The face then changes A's flux/depth before overflowing the full destination B.
    a->water_bed_mm.assign(64, std::numeric_limits<std::int32_t>::max());
    b->water_bed_mm.assign(64, 1); // Above sea level, so B's interior cannot drain the fault.
    b->water_depth_mm.assign(64, std::numeric_limits<std::int32_t>::max());
    const auto refused_a = CaptureWater(*a), refused_b = CaptureWater(*b);
    const auto encoded = ledger.encode();
    const auto telemetry = capture_telemetry(water);
    const auto authority = capture_authority(shield);
    const auto completed = capture_work(work);
    try {
        water.UpdateRegional(registry, chunks, ledger, schedule, work, 4 * kCapacity);
        FAIL() << "Expected a late shared-face depth overflow";
    } catch (const std::runtime_error& error) {
        EXPECT_STREQ(error.what(), "Regional water depth overflow");
    }
    EXPECT_EQ(CaptureWater(*a), refused_a);
    EXPECT_EQ(CaptureWater(*b), refused_b);
    EXPECT_EQ(ledger.encode(), encoded);
    EXPECT_EQ(capture_telemetry(water), telemetry);
    EXPECT_EQ(capture_authority(shield), authority);
    EXPECT_EQ(capture_work(work), completed);

    // Repair only the injected fault, preserving the complete topology and identities.
    a->water_bed_mm = valid_a.bed;
    b->water_bed_mm = valid_b.bed;
    b->water_depth_mm = valid_b.depth;
    ASSERT_EQ(chunks, roster);
    ASSERT_EQ(CaptureWater(*a), valid_a);
    ASSERT_EQ(CaptureWater(*b), valid_b);
    Systems::WaterSystem cold(nullptr, &reference_shield);
    cold.SetSimResolution(static_cast<int>(Systems::WaterDetailLevel::Medium));
    cold.SetHydrology(true, 0, 0);
    water.UpdateRegional(registry, chunks, ledger, schedule, work, 4 * kCapacity);
    cold.UpdateRegional(
        registry, reference, reference_ledger, schedule, reference_work, 4 * kCapacity);
    EXPECT_EQ(chunks, roster);
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        EXPECT_EQ(CaptureWater(*chunks[i]), CaptureWater(*reference[i]));
        EXPECT_EQ(chunks[i]->water_source_inputs, reference[i]->water_source_inputs);
    }
    EXPECT_EQ(save_bytes(chunks), save_bytes(reference));
    EXPECT_EQ(ledger.encode(), reference_ledger.encode());
    EXPECT_EQ(capture_work(work), capture_work(reference_work));
    EXPECT_EQ(capture_telemetry(water), capture_telemetry(cold));
    EXPECT_EQ(capture_authority(shield), capture_authority(reference_shield));
}

TEST_F(RegionalWaterFixture, ReadmittedCachedResidentIsValidatedBeforeAnyPublication) {
    Owner({0, 0});
    Owner({1, 0});
    chunks = {WaterChunk({31, 0, 0}), WaterChunk({32, 0, 0})};
    auto& records = const_cast<std::map<RegionKey, world::ActiveRegionRecord>&>(ledger.records());
    records.at({1, 0}).state = RegionState::Frozen;
    chunks[1]->water_depth_mm[0] = -1;
    Step(1, {{0, 0}}); // The frozen resident's payload is sealed, its geometry is cached.
    WorldClock clock;
    clock.set_tick(1);
    auto schedule = ledger.schedule(clock, std::span<const Vec3>{});
    records.at({1, 0}).state = RegionState::Active;
    schedule.due = {{0, 0}, {1, 0}};
    const auto a = CaptureWater(*chunks[0]), b = CaptureWater(*chunks[1]);
    const auto encoded = ledger.encode();
    const auto telemetry = water.regional_work();
    const auto revision = shield.far_lod_authority_revision();
    const auto completed_size = work.size();
    EXPECT_THROW(water.UpdateRegional(registry, chunks, ledger, schedule, work, 4 * kCapacity),
                 std::runtime_error);
    EXPECT_EQ(CaptureWater(*chunks[0]), a);
    EXPECT_EQ(CaptureWater(*chunks[1]), b);
    EXPECT_EQ(ledger.encode(), encoded);
    EXPECT_EQ(water.regional_work(), telemetry);
    EXPECT_EQ(shield.far_lod_authority_revision(), revision);
    EXPECT_EQ(work.size(), completed_size);
    chunks[1]->water_depth_mm[0] = 1000;
    EXPECT_NO_THROW(water.UpdateRegional(registry, chunks, ledger, schedule, work, 4 * kCapacity));
}

TEST_F(RegionalWaterFixture, UninitializedAndOldResolutionClaimsEarnPreparationAndOneKernel) {
    Owner({0, 0});
    auto fresh = std::make_shared<Chunk>(IVec3(0, 0, 0));
    fresh->set_state(ChunkState::Idle);
    chunks = {fresh, WaterChunk({1, 0, 0}, 17, 4)};
    water.SetHydrology(true, 1, 0);
    Step(2, {{0, 0}});
    const auto& result = water.regional_work();
    EXPECT_EQ(result.used_windows, 2u);
    EXPECT_EQ(result.visits.size(), 2u);
    EXPECT_EQ(result.preparation_cells, 128u);
    EXPECT_EQ(result.water_cells, 144u); // two kernels plus the same-owner face
    EXPECT_EQ(result.total_cells, 272u);
    EXPECT_LE(result.total_cells, 2 * kCapacity);
    EXPECT_TRUE(result.mass_ok);
    EXPECT_EQ(result.volume_after_qmm - result.volume_before_qmm,
              result.source_volume_qmm - result.sink_volume_qmm + result.preparation_volume_qmm);
    EXPECT_EQ(result.source_volume_qmm, result.source_mm * 16);
    for (const auto& chunk : chunks) {
        EXPECT_TRUE(chunk->has_water_sim.load());
        EXPECT_EQ(chunk->water_depth_mm.size(), 64u);
        EXPECT_TRUE(chunk->has_water_authority());
        EXPECT_EQ(chunk->water_revision(), 1u);
        EXPECT_EQ(chunk->sdf_provenance(), ChunkSdfProvenance::LoadedOrEdited);
        EXPECT_TRUE(chunk->is_voxel_data_dirty());
        EXPECT_FALSE(chunk->is_water_state_dirty());
    }
    ASSERT_EQ(work.size(), 1u);
    EXPECT_EQ(work.front().region_ticks, 1u);
    EXPECT_EQ(work.front().water_cells, 144u);
    EXPECT_EQ(shield.far_lod_authority_revision(), 2u);
    EXPECT_EQ(shield.far_lod_region_authority_revision(0, 0), 2u);
    EXPECT_EQ(shield.far_lod_region_authority_revision(-1, -1), 1u);
}

TEST_F(RegionalWaterFixture, CachedRiverSourcesMatchRebuildingAcrossTicksResizeAndWorldInputs) {
    Owner({0, 0});
    chunks = {WaterChunk({0, 0, 0}), WaterChunk({1, 0, 0})};
    std::vector<std::shared_ptr<Chunk>> reference{WaterChunk({0, 0, 0}), WaterChunk({1, 0, 0})};
    Systems::WaterSystem rebuilding(nullptr, &shield);
    water.SetHydrology(false, 0, 0);
    rebuilding.SetHydrology(false, 0, 0);
    auto params = shield.get_params();
    params.rivers_enabled = true;
    shield.set_params(params);
    for (std::uint64_t tick = 1; tick <= 12; ++tick) {
        SCOPED_TRACE(tick);
        if (tick == 4)
            shield.set_seed(42);
        if (tick == 7) {
            params.river_frequency *= 2;
            params.river_pv_min = -0.75f;
            params.river_pv_max = 0.75f;
            shield.set_params(params);
        }
        if (tick == 10) {
            params.rivers_enabled = false;
            shield.set_params(params);
        }
        const int resolution = tick < 6 ? 8 : 16;
        water.SetSimResolution(resolution);
        rebuilding.SetSimResolution(resolution);
        WorldClock clock;
        clock.set_tick(tick);
        const auto schedule = ledger.schedule(clock);
        auto reference_ledger = ledger;
        std::vector<world::RegionWork> actual_work, reference_work;
        for (const auto& chunk : chunks)
            chunk->is_water_sleeping.store(false);
        for (const auto& chunk : reference) {
            chunk->is_water_sleeping.store(false);
            // The original regional staging rebuilt this derived mask on every visit.
            chunk->water_src_mm.clear();
            chunk->water_source_inputs.reset();
        }
        const auto budget = 2 * (2 * resolution * resolution + 4 * resolution);
        water.UpdateRegional(registry, chunks, ledger, schedule, actual_work, budget);
        rebuilding.UpdateRegional(
            registry, reference, reference_ledger, schedule, reference_work, budget);
        for (std::size_t i = 0; i < chunks.size(); ++i) {
            EXPECT_EQ(CaptureWater(*chunks[i]), CaptureWater(*reference[i]));
            const auto& inputs = chunks[i]->water_source_inputs;
            ASSERT_TRUE(inputs.has_value());
            if (!inputs.has_value())
                return;
            EXPECT_EQ(inputs->seed, shield.get_seed());
        }
        EXPECT_EQ(ledger.encode(), reference_ledger.encode());
        EXPECT_EQ(water.regional_work().water_cells, rebuilding.regional_work().water_cells);
        EXPECT_EQ(water.regional_work().source_mm, rebuilding.regional_work().source_mm);
        ASSERT_EQ(actual_work.size(), reference_work.size());
        for (std::size_t i = 0; i < actual_work.size(); ++i) {
            EXPECT_EQ(actual_work[i].key, reference_work[i].key);
            EXPECT_EQ(actual_work[i].region_ticks, reference_work[i].region_ticks);
            EXPECT_EQ(actual_work[i].water_cells, reference_work[i].water_cells);
        }
    }
}

TEST(RegionalWater,
     EntirelyUninitializedOwnerEarnsOnePreparationAndKernelAtLegalSessionResolutions) {
    for (const int resolution : {4, 8, 16}) {
        SCOPED_TRACE(resolution);
        Systems::SHIELD_WorldSystem shield(nullptr, nullptr, Systems::TerrainGenParams{}, 1337);
        Systems::WaterSystem water(nullptr, &shield);
        water.SetHydrology(true, 0, 0);
        water.SetSimResolution(resolution);
        ActiveRegionLedger ledger;
        ledger.mark_edited({0, 0}, WorldClock{});
        auto chunk = std::make_shared<Chunk>(IVec3(0, 0, 0));
        chunk->set_state(ChunkState::Idle);
        std::vector<std::shared_ptr<Chunk>> resident{chunk};
        entt::registry registry;
        RegionSchedule schedule;
        schedule.due = {{0, 0}};
        std::vector<world::RegionWork> work;
        const auto cells = static_cast<std::uint64_t>(resolution * resolution);
        water.UpdateRegional(registry,
                             resident,
                             ledger,
                             schedule,
                             work,
                             2 * cells + 4 * static_cast<std::uint64_t>(resolution));
        EXPECT_EQ(water.regional_work().used_windows, 1u);
        EXPECT_EQ(water.regional_work().preparation_cells, cells);
        EXPECT_EQ(water.regional_work().water_cells, cells);
        EXPECT_TRUE(water.regional_work().mass_ok);
        EXPECT_TRUE(chunk->has_water_sim.load());
        EXPECT_EQ(chunk->current_water_resolution.load(), resolution);
        EXPECT_EQ(chunk->water_depth_mm.size(), cells);
        ASSERT_EQ(work.size(), 1u);
        EXPECT_EQ(work.front().water_cells, cells);
    }
}

TEST_F(RegionalWaterFixture, SavedUltraGridResizesWithExactPhysicalVolumeAccounting) {
    Owner({0, 0});
    chunks = {WaterChunk({0, 0, 0}, 1000, 32)};
    Step(1, {{0, 0}});
    EXPECT_EQ(chunks.front()->current_water_resolution.load(), 8);
    EXPECT_EQ(water.regional_work().preparation_cells, 64u);
    EXPECT_EQ(water.regional_work().volume_before_qmm, 1024 * 1000);
    EXPECT_EQ(water.regional_work().preparation_volume_qmm, 0);
    EXPECT_TRUE(water.regional_work().mass_ok);
}

TEST_F(RegionalWaterFixture, MalformedBedsFluxDepthMirrorsAndDuplicateIdentitiesRefuseAtomically) {
    Owner({0, 0});
    for (int fault = 0; fault < 8; ++fault) {
        SCOPED_TRACE(fault);
        auto a = WaterChunk({0, 0, 0});
        auto b = WaterChunk({1, 0, 0});
        if (fault == 0)
            b->water_bed_mm.pop_back();
        if (fault == 1)
            b->water_edge_flux.pop_back();
        if (fault == 2)
            b->water_depth_mm.front() = -1;
        if (fault == 3)
            b->water_flow_data.clear();
        if (fault == 4)
            b->current_water_resolution.store(3);
        if (fault == 5)
            b->has_water_sim.store(false);
        if (fault == 7)
            b->water_sim_terrain_height[0] = std::numeric_limits<float>::infinity();
        chunks = {a, b};
        if (fault == 6)
            chunks.push_back(b);
        const auto prior_a = CaptureWater(*a), prior_b = CaptureWater(*b);
        const auto prior_ledger = ledger.encode();
        const auto telemetry = water.regional_work();
        EXPECT_THROW(Step(4, {{0, 0}}), std::runtime_error);
        EXPECT_EQ(CaptureWater(*a), prior_a);
        EXPECT_EQ(CaptureWater(*b), prior_b);
        EXPECT_EQ(ledger.encode(), prior_ledger);
        EXPECT_EQ(water.regional_work(), telemetry);
        EXPECT_TRUE(work.empty());
    }
}

TEST(RegionalWater, ZeroThroughThreeWindowsPreserveInteriorAndSealUnfundedPairs) {
    for (const std::uint64_t windows : {0u, 1u, 2u, 3u}) {
        Systems::SHIELD_WorldSystem shield(nullptr, nullptr, Systems::TerrainGenParams{}, 1337);
        Systems::WaterSystem water(nullptr, &shield);
        water.SetSimResolution(static_cast<int>(Systems::WaterDetailLevel::Medium));
        water.SetHydrology(true, 1, 0);
        ActiveRegionLedger ledger;
        ledger.mark_edited({0, 0}, WorldClock{});
        ledger.mark_edited({1, 0}, WorldClock{});
        ASSERT_TRUE(ledger.set_water_debts({{{0, 0}, world::water_share::kQ32}}));
        std::vector<std::shared_ptr<Chunk>> chunks{WaterChunk({31, 0, 0}),
                                                   WaterChunk({32, 0, 0}, 0),
                                                   WaterChunk({31, 0, 1}),
                                                   WaterChunk({32, 0, 1}, 0)};
        const auto old = CaptureWater(*chunks[0]);
        RegionSchedule schedule;
        schedule.due = {{0, 0}, {1, 0}};
        std::vector<world::RegionWork> work;
        entt::registry registry;
        water.UpdateRegional(registry, chunks, ledger, schedule, work, windows * kCapacity);
        const auto& result = water.regional_work();
        EXPECT_EQ(result.global_windows, windows);
        EXPECT_EQ(result.border_windows, windows == 3 ? 2u : 0u);
        EXPECT_LE(result.border_windows + result.allocated_windows, windows);
        EXPECT_LE(result.border_windows + result.used_windows, windows);
        EXPECT_EQ(result.allocated_windows, result.used_windows + result.unused_windows);
        EXPECT_EQ(windows,
                  result.border_windows + result.allocated_windows + result.unallocated_windows);
        EXPECT_LE(result.total_cells, windows * kCapacity);
        std::uint64_t total_debt = 0;
        for (const auto& [key, debt] : ledger.water_debts()) {
            total_debt += debt;
            (void)key;
        }
        EXPECT_EQ(total_debt,
                  world::water_share::kQ32 +
                      (windows - result.border_windows) * world::water_share::kQ32 -
                      result.allocated_windows * world::water_share::kQ32);
        if (windows > 0) {
            EXPECT_GE(result.used_windows, 1u);
        } else {
            EXPECT_EQ(CaptureWater(*chunks[0]), old);
            EXPECT_TRUE(work.empty());
        }
        EXPECT_TRUE(result.mass_ok);
        EXPECT_FALSE(result.fixed_cohort_bound_known);
    }
}

TEST(RegionalWater, HugeGrantDiscardsCapacityWithoutRepeatingRainDampingSourceOrSleep) {
    const auto run = [](std::uint64_t windows) {
        Systems::SHIELD_WorldSystem shield(nullptr, nullptr, Systems::TerrainGenParams{}, 1337);
        Systems::WaterSystem water(nullptr, &shield);
        water.SetSimResolution(static_cast<int>(Systems::WaterDetailLevel::Medium));
        water.SetHydrology(true, 3, 2);
        ActiveRegionLedger ledger;
        ledger.mark_edited({0, 0}, WorldClock{});
        auto chunk = WaterChunk({0, 0, 0});
        chunk->water_edge_flux[0] = 100;
        chunk->ticks_below_threshold = 119;
        std::vector<std::shared_ptr<Chunk>> chunks{chunk};
        entt::registry registry;
        const auto e = registry.create();
        registry.emplace<Components::TransformComponent>(e).position = Vec3(1, 0, 1);
        registry.emplace<Components::WaterFlowSourceComponent>(e).flow_rate = 1.0f;
        RegionSchedule schedule;
        schedule.due = {{0, 0}};
        std::vector<world::RegionWork> work;
        water.UpdateRegional(registry, chunks, ledger, schedule, work, windows * kCapacity);
        const auto result = water.regional_work();
        EXPECT_EQ(result.allocated_windows, windows);
        EXPECT_EQ(result.used_windows, 1u);
        EXPECT_EQ(result.unused_windows, windows - 1);
        EXPECT_EQ(result.water_cells, 64u);
        EXPECT_TRUE(ledger.water_debts().empty());
        EXPECT_EQ(chunk->water_edge_flux[0], 78);
        EXPECT_EQ(result.source_mm, 64 * 3 + 8);
        EXPECT_EQ(result.sink_mm, 64 * 2);
        EXPECT_EQ(work.size(), 1u);
        return CaptureWater(*chunk);
    };
    EXPECT_EQ(run(1), run(100));
}

TEST_F(RegionalWaterFixture,
       ZeroAwakeOldDebtGrantsHaveNoVisitsOrCursorAdvanceAndAbsentDebtSurvives) {
    Owner({0, 0});
    Owner({4, 0});
    ledger.set_water_cursor({0, 0}, 999);
    ASSERT_TRUE(ledger.set_water_debts({{{0, 0}, 3 * world::water_share::kQ32}, {{4, 0}, 17}}));
    auto chunk = WaterChunk({0, 0, 0});
    chunk->is_water_sleeping.store(true);
    chunks = {chunk};
    const auto prior = CaptureWater(*chunk);
    Step(10, {{0, 0}});
    const auto& result = water.regional_work();
    EXPECT_EQ(result.allocated_windows, 3u);
    EXPECT_EQ(result.used_windows, 0u);
    EXPECT_EQ(result.unused_windows, 3u);
    EXPECT_EQ(result.unallocated_windows, 7u);
    EXPECT_EQ(ledger.water_debts(), (ActiveRegionLedger::WaterDebts{{{4, 0}, 17}}));
    EXPECT_EQ(ledger.records().at({0, 0}).water_cursor, 999u);
    EXPECT_EQ(CaptureWater(*chunk), prior);
    EXPECT_TRUE(work.empty());
}

TEST_F(RegionalWaterFixture,
       BorderOnlyPositiveFacesSharedCornerPreserveOrientationWakeAndUnionMass) {
    Owner({-1, 0});
    Owner({0, 0});
    auto positive = WaterChunk({-1, 0, 31}, 1000);
    auto east = WaterChunk({0, 0, 31}, 0);
    auto north = WaterChunk({-1, 0, 32}, 0);
    Owner({-1, 1});
    east->is_water_sleeping.store(true);
    north->is_water_sleeping.store(true);
    east->ticks_below_threshold = 119;
    north->ticks_below_threshold = 119;
    chunks = {north, east, positive};
    water.SetHydrology(true, 5, 1);
    Source({-.25f, 0, 511.75f}); // border-only sources/rain never execute a kernel
    Source({.25f, 0, 511.75f});
    const auto total = [](const auto& values) {
        std::int64_t sum = 0;
        for (const auto& chunk : values)
            sum += std::accumulate(
                chunk->water_depth_mm.begin(), chunk->water_depth_mm.end(), std::int64_t{0});
        return sum;
    };
    const auto before = total(chunks);
    Step(4, {});
    EXPECT_TRUE(water.regional_work().visits.empty());
    EXPECT_EQ(water.regional_work().border_windows, 4u);
    EXPECT_EQ(water.regional_work().water_cells, 32u);
    EXPECT_EQ(water.regional_work().source_mm, 0);
    EXPECT_EQ(positive->water_depth_mm[63], 906); // +Z 48 then +X 46 in sorted region-key order
    EXPECT_EQ(east->water_depth_mm[56], 46);
    EXPECT_EQ(north->water_depth_mm[7], 48);
    EXPECT_EQ(east->ticks_below_threshold, 0);
    EXPECT_EQ(north->ticks_below_threshold, 0);
    EXPECT_FALSE(east->is_water_sleeping.load());
    EXPECT_EQ(total(chunks), before);
    EXPECT_TRUE(water.regional_work().mass_ok);
    EXPECT_TRUE(ledger.water_debts().empty());
    ASSERT_EQ(work.size(), 3u);
    EXPECT_EQ(work[0].water_cells, 16u); // (-1,0) has two separate face endpoints
    EXPECT_EQ(work[1].water_cells, 8u);
    EXPECT_EQ(work[2].water_cells, 8u);
    for (const auto& contribution : work)
        EXPECT_EQ(contribution.region_ticks, 1u);
}

TEST_F(RegionalWaterFixture, CommonBorderRunsOffInteriorPhaseAndFrozenOrResizingNeighborIsSealed) {
    Owner({0, 0});
    Owner({1, 0});
    Owner({0, 1});
    auto& records = const_cast<std::map<RegionKey, world::ActiveRegionRecord>&>(ledger.records());
    records.at({0, 0}).state = RegionState::Reduced;
    records.at({0, 0}).cadence_shift = 2;
    records.at({0, 0}).cadence_phase = 3;
    records.at({1, 0}).state = RegionState::Reduced;
    records.at({1, 0}).cadence_shift = 1;
    records.at({1, 0}).cadence_phase = 1;
    auto a = WaterChunk({31, 0, 31}), b = WaterChunk({32, 0, 31}, 0);
    auto frozen = WaterChunk({31, 0, 32}, 0);
    records.at({0, 1}).state = RegionState::Frozen;
    const auto frozen_before = CaptureWater(*frozen);
    chunks = {a, b, frozen};
    Step(4, {}); // common phase at t=0; neither interior is due
    EXPECT_EQ(water.regional_work().border_windows, 2u);
    EXPECT_EQ(water.regional_work().used_windows, 0u);
    EXPECT_EQ(CaptureWater(*frozen), frozen_before);
    EXPECT_EQ(b->water_depth_mm[0], 48);
}

TEST_F(RegionalWaterFixture,
       FloorMappedResidentColumnSourceAndOneHopWakeExcludeUnselectedAndLoading) {
    Owner({-1, -1});
    auto target = WaterChunk({-1, 0, -1}, 0);
    target->is_water_sleeping.store(true);
    auto loading = WaterChunk({-2, 0, -1});
    loading->set_state(ChunkState::Loading);
    chunks = {target, loading};
    const auto untouched = CaptureWater(*loading);
    // Keep the injected eight millimetres in a basin so this mapping assertion
    // remains independent of the interior flux step that follows source injection.
    target->water_bed_mm[63] = 900;
    target->water_level_data[63] = 0.9f;
    target->water_sim_terrain_height[63] = 0.9f;
    Source({-.25f, -.25f, -.25f}); // direct y=-1 missing; resident y=0, floor cell (7,7)
    Step(1, {{-1, -1}});
    EXPECT_EQ(target->water_depth_mm[63], 8);
    EXPECT_EQ(water.regional_work().source_mm, 8);
    EXPECT_EQ(CaptureWater(*loading), untouched);
    EXPECT_EQ(water.regional_work().visits, (std::vector<ChunkID>{target->get_id()}));
}

TEST_F(RegionalWaterFixture, SourceRangesMatchFullScanTotalsAndLeaveUndueTargetsUntouched) {
    Owner({0, 0});
    Owner({1, 0});
    Owner({2, 0});
    chunks = {WaterChunk({0, 0, 0}, 0),
              WaterChunk({2, 0, 0}, 0),
              WaterChunk({32, 0, 0}, 0),
              WaterChunk({34, 0, 0}, 0),
              WaterChunk({64, 0, 0}, 0),
              WaterChunk({66, 0, 0}, 0),
              WaterChunk({68, 0, 0}, 0)};
    struct SourceCase {
        std::size_t target, cell;
        float rate;
        std::int32_t millimetres;
    };
    // Deliberately interleave targets, cells and amounts, including a repeated cell.
    constexpr SourceCase sources[] = {{3, 63, 5.0f, 42},
                                      {5, 9, 2.0f, 17},
                                      {1, 0, 3.0f, 25},
                                      {2, 0, 4.0f, 33},
                                      {4, 63, 1.0f, 8},
                                      {1, 9, 1.0f, 8},
                                      {1, 0, 1.0f, 8}};
    for (const auto& source : sources) {
        auto& chunk = *chunks[source.target];
        chunk.water_bed_mm[source.cell] = 900;
        chunk.water_level_data[source.cell] = 0.9f;
        chunk.water_sim_terrain_height[source.cell] = 0.9f;
        const auto coords = chunk.get_coords();
        Source({static_cast<float>(coords.x * CHUNK_SIZE_X + 2 * static_cast<int>(source.cell % 8) +
                                   1),
                0.0f,
                static_cast<float>(2 * static_cast<int>(source.cell / 8) + 1)},
               source.rate);
    }
    const auto old_first_undue = CaptureWater(*chunks[2]);
    const auto old_second_undue = CaptureWater(*chunks[3]);
    Step(5, {{0, 0}, {2, 0}});

    // Isolated dry basins retain every injection with zero flux or sinks. The old
    // full-list target filter therefore gives an independent exact depth oracle.
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        SCOPED_TRACE(i);
        std::vector<std::int32_t> expected(64, 0);
        if (i != 2 && i != 3) {
            for (const auto& source : sources) {
                if (source.target == i)
                    expected[source.cell] += source.millimetres;
            }
        }
        EXPECT_EQ(chunks[i]->water_depth_mm, expected);
        EXPECT_EQ(chunks[i]->water_edge_flux, (std::vector<std::int32_t>(128, 0)));
    }
    EXPECT_EQ(CaptureWater(*chunks[2]), old_first_undue);
    EXPECT_EQ(CaptureWater(*chunks[3]), old_second_undue);
    const auto& result = water.regional_work();
    EXPECT_EQ(result.used_windows, 5u);
    EXPECT_EQ(result.water_cells, 5 * 64u);
    EXPECT_EQ(result.source_mm, 66);
    EXPECT_EQ(result.sink_mm, 0);
    EXPECT_EQ(result.source_volume_qmm, 66 * 16);
    EXPECT_EQ(result.volume_before_qmm, 0);
    EXPECT_EQ(result.volume_after_qmm, 66 * 16);
    EXPECT_TRUE(result.mass_ok);
    EXPECT_EQ(water.debug_sources_seen(), 7);
    EXPECT_EQ(water.debug_source_injected_mm(), 66);
    EXPECT_EQ(water.dbg_last_source_mm(), 66);
    EXPECT_EQ(result.visits.size(), 5u);
}

TEST_F(RegionalWaterFixture,
       SnapshotWakeCannotCascadeAndSourceCannotVisitUnknownFrozenOrUnselected) {
    Owner({0, 0});
    Owner({1, 0});
    auto a = WaterChunk({0, 0, 0}), b = WaterChunk({1, 0, 0}), c = WaterChunk({2, 0, 0});
    b->is_water_sleeping.store(true);
    c->is_water_sleeping.store(true);
    auto unknown = WaterChunk({64, 0, 0});
    auto frozen = WaterChunk({32, 0, 0});
    auto& records = const_cast<std::map<RegionKey, world::ActiveRegionRecord>&>(ledger.records());
    records.at({1, 0}).state = RegionState::Frozen;
    const auto old_c = CaptureWater(*c), old_unknown = CaptureWater(*unknown),
               old_frozen = CaptureWater(*frozen);
    chunks = {a, b, c, unknown, frozen, nullptr};
    Source({1025, 0, 1});
    Source({513, 0, 1});
    Step(2, {{0, 0}});
    EXPECT_EQ(water.regional_work().used_windows, 2u);
    EXPECT_EQ(CaptureWater(*c), old_c);
    EXPECT_EQ(CaptureWater(*unknown), old_unknown);
    EXPECT_EQ(CaptureWater(*frozen), old_frozen);
    EXPECT_FALSE(b->is_water_sleeping.load());
}

TEST(RegionalWater, PermutedResidentsAndSourcesRetainCursorDebtPhysicalOrderAndExactWork) {
    const auto run = [](bool reversed) {
        Systems::SHIELD_WorldSystem shield(nullptr, nullptr, Systems::TerrainGenParams{}, 1337);
        Systems::WaterSystem water(nullptr, &shield);
        water.SetSimResolution(static_cast<int>(Systems::WaterDetailLevel::Medium));
        water.SetHydrology(true, 1, 0);
        ActiveRegionLedger ledger;
        ledger.mark_edited({0, 0}, WorldClock{});
        ledger.mark_edited({1, 0}, WorldClock{});
        ledger.set_water_cursor({0, 0}, 1);
        std::vector<std::shared_ptr<Chunk>> chunks{WaterChunk({31, 0, 0}),
                                                   WaterChunk({32, 0, 0}, 0),
                                                   WaterChunk({31, 0, 1}),
                                                   WaterChunk({32, 0, 1}, 0)};
        entt::registry registry;
        std::vector<Vec3> positions{Vec3(500, 0, 1), Vec3(513, 0, 17)};
        if (reversed) {
            std::reverse(chunks.begin(), chunks.end());
            std::reverse(positions.begin(), positions.end());
        }
        for (const auto& position : positions) {
            const auto e = registry.create();
            registry.emplace<Components::TransformComponent>(e).position = position;
            registry.emplace<Components::WaterFlowSourceComponent>(e).flow_rate = 1;
        }
        RegionSchedule schedule;
        schedule.due = {{1, 0}, {0, 0}};
        std::vector<world::RegionWork> work;
        water.UpdateRegional(registry, chunks, ledger, schedule, work, 10 * kCapacity);
        const auto result = water.regional_work();
        EXPECT_EQ(result.border_windows, 4u);
        EXPECT_EQ(result.allocated_windows, 6u);
        EXPECT_EQ(result.used_windows, 4u);
        EXPECT_EQ(result.unused_windows, 2u);
        EXPECT_EQ(result.water_cells, 320u); // all four kernels and all four physical faces
        EXPECT_EQ(result.source_mm, 4 * 64 + 16);
        EXPECT_TRUE(result.mass_ok);
        EXPECT_EQ(std::set<ChunkID>(result.visits.begin(), result.visits.end()).size(), 4u);
        std::sort(chunks.begin(), chunks.end(), [](const auto& a, const auto& b) {
            return a->get_id() < b->get_id();
        });
        std::vector<WaterState> states;
        for (const auto& c : chunks)
            states.push_back(CaptureWater(*c));
        return std::make_tuple(result, ledger.encode(), states);
    };
    EXPECT_EQ(run(false), run(true));
}

TEST_F(RegionalWaterFixture, CheckedSourceAllocatorAndLateSeamOverflowRefuseWholePlan) {
    Owner({0, 0});
    Owner({1, 0});
    for (int fault = 0; fault < 4; ++fault) {
        SCOPED_TRACE(fault);
        registry.clear();
        auto a = WaterChunk({31, 0, 0}), b = WaterChunk({32, 0, 0});
        chunks = {a, b};
        if (fault == 0) {
            a->water_depth_mm.assign(64, std::numeric_limits<std::int32_t>::max());
            water.SetHydrology(true, 1, 0);
        } else if (fault == 1) {
            water.SetHydrology(true, 0, 0);
            a->water_depth_mm[0] = std::numeric_limits<std::int32_t>::max();
            Source({497, 0, 1});
        } else if (fault == 2) {
            ASSERT_TRUE(ledger.set_water_debts({{{0, 0}, world::water_share::kMaxDebt}}));
        } else {
            ASSERT_TRUE(ledger.set_water_debts({}));
            a->water_bed_mm.assign(64, std::numeric_limits<std::int32_t>::max());
            b->water_bed_mm.assign(64, 0);
            b->water_depth_mm.assign(64, std::numeric_limits<std::int32_t>::max());
            Source({497, 0, 1}); // staged source and kernel succeed before the face refuses
        }
        const auto old_a = CaptureWater(*a), old_b = CaptureWater(*b);
        const auto old_ledger = ledger.encode();
        const auto telemetry = water.regional_work();
        const auto revision = shield.far_lod_region_authority_revision(0, 0);
        EXPECT_THROW(Step(4, {{0, 0}}), std::runtime_error);
        EXPECT_EQ(CaptureWater(*a), old_a);
        EXPECT_EQ(CaptureWater(*b), old_b);
        EXPECT_EQ(ledger.encode(), old_ledger);
        EXPECT_EQ(water.regional_work(), telemetry);
        EXPECT_EQ(shield.far_lod_region_authority_revision(0, 0), revision);
        EXPECT_TRUE(work.empty());
    }
}

TEST_F(RegionalWaterFixture, RepeatedTickRefusesAndAllSameOwnerFacesFitCellBudget) {
    Owner({0, 0});
    chunks = {
        WaterChunk({0, 0, 0}), WaterChunk({1, 0, 0}), WaterChunk({0, 0, 1}), WaterChunk({1, 0, 1})};
    Step(4, {{0, 0}});
    EXPECT_EQ(water.regional_work().water_cells, 320u);
    EXPECT_LE(water.regional_work().total_cells, 4 * kCapacity);
    const auto old = CaptureWater(*chunks[0]);
    const auto ledger_before = ledger.encode();
    const auto output = water.regional_work();
    EXPECT_THROW(Step(4, {{0, 0}}), std::runtime_error);
    EXPECT_EQ(CaptureWater(*chunks[0]), old);
    EXPECT_EQ(ledger.encode(), ledger_before);
    EXPECT_EQ(water.regional_work(), output);
}

TEST_F(RegionalWaterFixture, ResizeIsExcludedFromCrossBorderReservationUntilItsBudgetedVisit) {
    Owner({0, 0});
    Owner({1, 0});
    auto a = WaterChunk({31, 0, 0}, 1000, 4), b = WaterChunk({32, 0, 0}, 1000, 4);
    const auto old_b = CaptureWater(*b);
    chunks = {a, b};
    Step(3, {{0, 0}});
    EXPECT_EQ(water.regional_work().border_windows, 0u);
    EXPECT_EQ(water.regional_work().used_windows, 1u);
    EXPECT_EQ(water.regional_work().preparation_cells, 64u);
    EXPECT_EQ(a->current_water_resolution.load(), 8);
    EXPECT_EQ(CaptureWater(*b), old_b);
}

TEST_F(RegionalWaterFixture,
       WeatherRainConservativelyAdmitsSleepingReadyGridsButDoesNotInjectIrrigation) {
    Owner({0, 0});
    auto a = WaterChunk({0, 0, 0}, 0);
    a->is_water_sleeping.store(true);
    chunks = {a};
    const auto e = registry.create();
    registry.emplace<Components::TransformComponent>(e).position = Vec3(1, 0, 1);
    registry.emplace<Components::WaterSourceComponent>(e).output = 65535;
    Systems::WeatherSystem weather(1337);
    water.SetWeatherRain(&weather, 1);
    Step(1, {{0, 0}});
    EXPECT_EQ(water.regional_work().used_windows, 1u);
    EXPECT_EQ(water.debug_source_injected_mm(), 0);
    EXPECT_EQ(water.debug_sources_seen(), 0);
    EXPECT_LE(water.regional_work().source_mm, 64); // weather only, never reinterpret u16 output
}

TEST_F(RegionalWaterFixture, WorldLimitFacesDoNotWrapPackedIdentityAndBadCoordinateRefuses) {
    Owner({32767, 0});
    Owner({-32768, 0});
    auto high = WaterChunk({1048575, 0, 0});
    auto low = WaterChunk({-1048576, 0, 0}, 0);
    const auto old_high = CaptureWater(*high), old_low = CaptureWater(*low);
    chunks = {high, low};
    Step(4, {});
    EXPECT_EQ(water.regional_work().border_windows, 0u);
    EXPECT_EQ(CaptureWater(*high), old_high);
    EXPECT_EQ(CaptureWater(*low), old_low);
    chunks.push_back(WaterChunk({1048576, 0, 0}));
    const auto encoded = ledger.encode();
    Systems::WaterSystem fresh(nullptr, &shield);
    fresh.SetSimResolution(static_cast<int>(Systems::WaterDetailLevel::Medium));
    RegionSchedule invalid;
    invalid.tick = ledger.tick();
    EXPECT_THROW(fresh.UpdateRegional(registry, chunks, ledger, invalid, work, 4 * kCapacity),
                 std::runtime_error);
    EXPECT_EQ(ledger.encode(), encoded);
}

TEST_F(RegionalWaterFixture,
       SourceConversionAndInternalDestinationOverflowRefuseBeforePublication) {
    Owner({0, 0});
    auto a = WaterChunk({0, 0, 0});
    chunks = {a};
    Source({1, 0, 1}, std::numeric_limits<float>::infinity());
    auto old = CaptureWater(*a);
    EXPECT_THROW(Step(1, {{0, 0}}), std::runtime_error);
    EXPECT_EQ(CaptureWater(*a), old);
    registry.clear();
    a->water_depth_mm.assign(64, std::numeric_limits<std::int32_t>::max());
    a->water_bed_mm.assign(64, std::numeric_limits<std::int32_t>::max());
    a->water_bed_mm[0] = 0; // neighbors transfer toward an already full destination
    old = CaptureWater(*a);
    const auto encoded = ledger.encode();
    EXPECT_THROW(Step(1, {{0, 0}}), std::runtime_error);
    EXPECT_EQ(CaptureWater(*a), old);
    EXPECT_EQ(ledger.encode(), encoded);
    EXPECT_TRUE(work.empty());
}

TEST_F(RegionalWaterFixture,
       CursorDebtAndCandidateMembershipResumeFromCanonicalLedgerWithoutCatchUp) {
    Owner({0, 0});
    Owner({4, 0});
    ledger.pin({0, 0}, true, WorldClock{});
    ledger.set_water_cursor({0, 0}, 5);
    ASSERT_TRUE(ledger.set_water_debts({{{4, 0}, 19}}));
    chunks = {WaterChunk({0, 0, 0}), WaterChunk({1, 0, 0}), WaterChunk({2, 0, 0})};
    Step(1, {{0, 0}});
    EXPECT_EQ(water.regional_work().visits, (std::vector<ChunkID>{chunks[2]->get_id()}));
    EXPECT_EQ(ledger.records().at({0, 0}).water_cursor, 0u);
    EXPECT_EQ(ledger.water_debts().at({4, 0}), 19u); // absent due claim untouched
    const auto before = ledger.encode();
    ActiveRegionLedger restored;
    std::string error;
    ASSERT_TRUE(ActiveRegionLedger::decode(before, restored, error)) << error;
    EXPECT_EQ(restored.encode(), before);
    // No ticking on restore; next due tick uses the same cursor/candidate membership.
    const std::span<const Vec3> no_anchors;
    auto schedule = ledger.schedule(WorldClock(1), no_anchors);
    auto restored_schedule = restored.schedule(WorldClock(1), no_anchors);
    schedule.due = {{0, 0}};
    restored_schedule.due = {{0, 0}};
    Systems::WaterSystem resumed(nullptr, &shield);
    resumed.SetSimResolution(static_cast<int>(Systems::WaterDetailLevel::Medium));
    resumed.SetHydrology(true, 0, 0);
    std::vector<std::shared_ptr<Chunk>> copies;
    for (const auto& c : chunks) {
        auto copy = WaterChunk(c->get_coords());
        copy->water_depth_mm = c->water_depth_mm;
        copy->water_bed_mm = c->water_bed_mm;
        copy->water_edge_flux = c->water_edge_flux;
        copy->ticks_below_threshold = c->ticks_below_threshold;
        copies.push_back(copy);
    }
    std::vector<world::RegionWork> resumed_work;
    water.UpdateRegional(registry, chunks, ledger, schedule, work, kCapacity);
    resumed.UpdateRegional(registry, copies, restored, restored_schedule, resumed_work, kCapacity);
    EXPECT_EQ(water.regional_work(), resumed.regional_work());
    EXPECT_EQ(ledger.encode(), restored.encode());
    EXPECT_EQ(chunks[0]->water_depth_mm, copies[0]->water_depth_mm);
    EXPECT_FALSE(water.regional_work().fixed_cohort_bound_known);
}

TEST_F(RegionalWaterFixture, WindowCapAndDuplicateOrUnknownDueOwnersRefuseAtomicPlan) {
    Owner({0, 0});
    chunks = {WaterChunk({0, 0, 0})};
    const auto before = CaptureWater(*chunks.front());
    const auto encoded = ledger.encode();
    const auto telemetry = water.regional_work();
    EXPECT_THROW(Step(world::water_share::kMaxWindowBudget + 1, {{0, 0}}), std::runtime_error);
    EXPECT_THROW(Step(1, {{0, 0}, {0, 0}}), std::runtime_error);
    EXPECT_THROW(Step(1, {{9, 9}}), std::runtime_error);
    EXPECT_EQ(CaptureWater(*chunks.front()), before);
    EXPECT_EQ(ledger.encode(), encoded);
    EXPECT_EQ(water.regional_work(), telemetry);
    EXPECT_TRUE(work.empty());
}

TEST(RegionalWaterPersistence, StrictDecodeRejectsMalformedAuthorityAndKeepsLegacyFluxRepair) {
    WorldStreamingState state;
    ASSERT_TRUE(state.insert_chunk(WaterChunk({0, 0, 0})));
    ASSERT_TRUE(state.insert_chunk(WaterChunk({1, 0, 0}, 500, 4)));
    const auto original =
        nlohmann::json::parse(Persistence::SerializeWorldStreamingStateSnapshotJson(state));
    for (int fault = 0; fault < 8; ++fault) {
        SCOPED_TRACE(fault);
        auto snapshot = original;
        auto& malformed = snapshot.at("chunks").at(1); // fail after a valid record
        switch (fault) {
            case 0:
                malformed.erase("water_edge_flux");
                break;
            case 1:
                malformed.at("water_bed_mm").erase(0);
                break;
            case 2:
                malformed.at("water_depth_mm").at(0) = -1;
                break;
            case 3:
                malformed.at("water_edge_flux").at(0) = std::uint64_t{1} << 32;
                break;
            case 4:
                malformed.at("water_bed_mm").at(0) = -2147483649ll;
                break;
            case 5:
                malformed.at("water_state").at("current_water_resolution") = 3;
                break;
            case 6:
                malformed.at("water_flow_data").clear();
                break;
            case 7:
                malformed.at("coords").at("x") = (std::uint64_t{1} << 32) + 1;
                break; // would narrow to the original x=1 and preserve its packed ID
        }
        WorldStreamingState restored;
        std::vector<std::string> errors;
        EXPECT_FALSE(Persistence::LoadWorldStreamingStateSnapshotJson(
            snapshot.dump(), restored, errors, true));
        EXPECT_EQ(restored.size(), 0u); // no partial authority adoption
        EXPECT_FALSE(errors.empty());
        if (fault == 0) {
            errors.clear();
            ASSERT_TRUE(Persistence::LoadWorldStreamingStateSnapshotJson(
                snapshot.dump(), restored, errors));
            EXPECT_EQ(restored.find_chunk(Chunk::calculate_id({1, 0, 0}))->water_edge_flux,
                      std::vector<std::int32_t>(32, 0));
        }
    }
    WorldStreamingState restored;
    std::vector<std::string> errors;
    ASSERT_TRUE(
        Persistence::LoadWorldStreamingStateSnapshotJson(original.dump(), restored, errors, true));
    const auto old_resolution = restored.find_chunk(Chunk::calculate_id({1, 0, 0}));
    ASSERT_NE(old_resolution, nullptr);
    EXPECT_EQ(old_resolution->current_water_resolution.load(), 4);
    EXPECT_EQ(CaptureWater(*old_resolution),
              CaptureWater(*state.find_chunk(old_resolution->get_id())));
}

void ReloadWaterResidents(std::vector<std::shared_ptr<Chunk>>& chunks,
                          std::vector<std::shared_ptr<Chunk>>& reference) {
    WorldStreamingState saved;
    for (const auto& chunk : chunks)
        ASSERT_TRUE(saved.insert_chunk(chunk));
    const auto bytes = Persistence::SerializeWorldStreamingStateSnapshotJson(saved);
    WorldStreamingState loaded, reference_loaded;
    std::vector<std::string> errors;
    ASSERT_TRUE(Persistence::LoadWorldStreamingStateSnapshotJson(bytes, loaded, errors, true));
    ASSERT_TRUE(
        Persistence::LoadWorldStreamingStateSnapshotJson(bytes, reference_loaded, errors, true));
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        const auto id = chunks[i]->get_id();
        chunks[i] = loaded.find_chunk(id);
        reference[i] = reference_loaded.find_chunk(id);
        ASSERT_NE(chunks[i], nullptr);
        ASSERT_NE(reference[i], nullptr);
    }
}

void RegionalWaterFixture::CheckColdStagingTransactions() {
    ledger = ActiveRegionLedger({0, 1, 2});
    Owner({0, 0});
    Owner({1, 0});
    chunks = {WaterChunk({31, 0, 0}), WaterChunk({32, 0, 0})};
    std::vector<std::shared_ptr<Chunk>> reference{WaterChunk({31, 0, 0}), WaterChunk({32, 0, 0})};
    Systems::SHIELD_WorldSystem reference_shield(
        nullptr, nullptr, Systems::TerrainGenParams{}, 1337);
    for (std::uint64_t tick = 1; tick <= 12; ++tick) {
        SCOPED_TRACE(tick);
        // A fresh WaterSystem has no retained transaction buffers.
        Systems::WaterSystem cold(nullptr, &reference_shield);
        const int resolution = tick < 5 ? 8 : 16;
        water.SetSimResolution(resolution);
        cold.SetSimResolution(resolution);
        water.SetHydrology(true, 1, 0);
        cold.SetHydrology(true, 1, 0);
        WorldClock clock;
        clock.set_tick(tick);
        // Pressure degrades owners through Reduced/Frozen; edits wake them again.
        if (tick == 7 || tick == 10) {
            WorldClock edit_clock;
            edit_clock.set_tick(tick - 1);
            ledger.mark_edited({0, 0}, edit_clock);
            ledger.mark_edited({1, 0}, edit_clock);
        }
        const std::vector<world::RegionWork> pressure{{{0, 0}, 1}, {{1, 0}, 1}};
        const auto schedule = ledger.schedule(
            clock, std::span<const Vec3>{}, pressure, [](RegionKey) { return std::uint64_t{123}; });
        auto reference_ledger = ledger;
        std::vector<world::RegionWork> actual_work, reference_work;
        if (tick == 3) {
            // Both transactions refuse after staging; retry must discard partial workspace state.
            chunks[0]->water_depth_mm.assign(64, std::numeric_limits<std::int32_t>::max());
            reference[0]->water_depth_mm = chunks[0]->water_depth_mm;
            const auto before = CaptureWater(*chunks[0]);
            EXPECT_THROW(
                water.UpdateRegional(registry, chunks, ledger, schedule, actual_work, 4096),
                std::runtime_error);
            EXPECT_THROW(cold.UpdateRegional(
                             registry, reference, reference_ledger, schedule, reference_work, 4096),
                         std::runtime_error);
            EXPECT_EQ(CaptureWater(*chunks[0]), before);
            EXPECT_EQ(ledger.encode(), reference_ledger.encode());
            EXPECT_TRUE(actual_work.empty());
            chunks[0] = WaterChunk({31, 0, 0}, 600);
            reference[0] = WaterChunk({31, 0, 0}, 600);
        }
        if (tick == 8) {
            ASSERT_NO_FATAL_FAILURE(ReloadWaterResidents(chunks, reference));
        }
        if (tick == 9) {
            chunks.pop_back();
            reference.pop_back();
        }
        if (tick == 10) {
            chunks.push_back(WaterChunk({32, 0, 0}, 111, 16));
            reference.push_back(WaterChunk({32, 0, 0}, 111, 16));
        }
        // A bed edit on a retained resident must replace the workspace's old bed.
        chunks[0]->water_bed_mm[0] = 1200 + static_cast<std::int32_t>(tick);
        reference[0]->water_bed_mm[0] = chunks[0]->water_bed_mm[0];
        water.UpdateRegional(registry, chunks, ledger, schedule, actual_work, 4096);
        cold.UpdateRegional(registry, reference, reference_ledger, schedule, reference_work, 4096);
        for (std::size_t i = 0; i < chunks.size(); ++i)
            EXPECT_EQ(CaptureWater(*chunks[i]), CaptureWater(*reference[i]));
        WorldStreamingState actual_save, reference_save;
        for (std::size_t i = 0; i < chunks.size(); ++i) {
            ASSERT_TRUE(actual_save.insert_chunk(chunks[i]));
            ASSERT_TRUE(reference_save.insert_chunk(reference[i]));
        }
        EXPECT_EQ(Persistence::SerializeWorldStreamingStateSnapshotJson(actual_save),
                  Persistence::SerializeWorldStreamingStateSnapshotJson(reference_save));
        EXPECT_EQ(ledger.encode(), reference_ledger.encode());
        EXPECT_EQ(water.regional_work(), cold.regional_work());
        ASSERT_EQ(actual_work.size(), reference_work.size());
        for (std::size_t i = 0; i < actual_work.size(); ++i) {
            EXPECT_EQ(actual_work[i].key, reference_work[i].key);
            EXPECT_EQ(actual_work[i].region_ticks, reference_work[i].region_ticks);
            EXPECT_EQ(actual_work[i].water_cells, reference_work[i].water_cells);
        }
        EXPECT_EQ(shield.far_lod_authority_revision(),
                  reference_shield.far_lod_authority_revision());
    }
}

void RegionalWaterFixture::CheckFlatStagingOracle() {
    // Independent closed-form oracle for the pre-reuse semantics: a flat isolated
    // grid has zero surface differences and therefore zero flux. Each cell gains
    // exactly the uniform rain plus its cached source, with no sink above sea.
    // A fresh staging chunk omitted sources in finite mode and copied them in
    // classic mode. Construct expected state from those rules, never UpdateRegional.
    // Reuse the already warm system and resident identity, including a replacement
    // at another resolution, to check complete payload overwrite and source clearing.
    ledger = ActiveRegionLedger{};
    std::int32_t expected_depth = 700;
    int expected_mesh_ticks = 0;
    std::uint32_t expected_revision = 0;
    const auto& params = shield.get_params();
    const Chunk::WaterSourceInputs inputs{shield.get_seed(),
                                          params.rivers_enabled,
                                          params.river_frequency,
                                          params.river_pv_min,
                                          params.river_pv_max};
    for (std::uint64_t tick = 13; tick <= 24; ++tick) {
        SCOPED_TRACE(tick);
        const int resolution = tick < 19 ? 8 : 16;
        if (tick == 13 || tick == 19) {
            expected_depth = 700;
            expected_mesh_ticks = 0;
            expected_revision = 0;
            chunks = {WaterChunk({31, 0, 0}, expected_depth, resolution)};
        }
        const auto cells = static_cast<std::size_t>(resolution * resolution);
        const bool finite = tick % 2 == 0;
        water.SetSimResolution(resolution);
        water.SetHydrology(finite, 1, 0);
        // The checked classic kernel accepts a cache with these matching inputs.
        // Finite staging must discard it even when last tick used classic mode.
        chunks[0]->water_src_mm.assign(cells, 3);
        chunks[0]->water_source_inputs = inputs;
        WorldClock edit_clock, clock;
        edit_clock.set_tick(tick - 1);
        clock.set_tick(tick);
        ledger.mark_edited({0, 0}, edit_clock);
        const auto schedule = ledger.schedule(clock, std::span<const Vec3>{});
        std::vector<world::RegionWork> actual_work;
        water.UpdateRegional(registry, chunks, ledger, schedule, actual_work, 4096);

        const std::int32_t added = finite ? 1 : 4;
        expected_depth += added;
        if (!finite)
            ++expected_mesh_ticks; // Only changes greater than one mm advance remeshing.
        auto expected = WaterChunk({31, 0, 0}, expected_depth, resolution);
        expected->water_level_data.assign(cells,
                                          static_cast<float>(1000 + expected_depth) / 1000.0f);
        expected->max_water_delta_last_tick = static_cast<float>(added) / 1000.0f;
        expected->water_mesh_dirty_ticks = expected_mesh_ticks;
        if (!finite) {
            expected->water_src_mm.assign(cells, 3);
            expected->water_source_inputs = inputs;
        }
        ++expected_revision;
        for (std::uint32_t i = 0; i < expected_revision; ++i)
            expected->mark_water_state_dirty();
        expected->set_water_authority();
        expected->mark_sdf_loaded_or_edited();
        expected->mark_dirty_for_save();
        expected->clear_water_state_dirty();
        EXPECT_EQ(CaptureWater(*chunks[0]), CaptureWater(*expected));
        EXPECT_EQ(chunks[0]->water_source_inputs, expected->water_source_inputs);
        WorldStreamingState actual_save, expected_save;
        ASSERT_TRUE(actual_save.insert_chunk(chunks[0]));
        ASSERT_TRUE(expected_save.insert_chunk(expected));
        EXPECT_EQ(Persistence::SerializeWorldStreamingStateSnapshotJson(actual_save),
                  Persistence::SerializeWorldStreamingStateSnapshotJson(expected_save));
        EXPECT_EQ(water.regional_work().source_mm, added * static_cast<std::int64_t>(cells));
        EXPECT_EQ(water.regional_work().sink_mm, 0);
        EXPECT_EQ(water.regional_work().water_cells, cells);
        EXPECT_TRUE(water.regional_work().mass_ok);
        ASSERT_EQ(actual_work.size(), 1u);
        EXPECT_EQ(actual_work[0].key, (RegionKey{0, 0}));
        EXPECT_EQ(actual_work[0].water_cells, cells);
    }
}

} // namespace

TEST_F(RegionalWaterFixture, ReusedStagingMatchesIndependentOracleAndColdTransactions) {
    ASSERT_NO_FATAL_FAILURE(CheckColdStagingTransactions());
    ASSERT_NO_FATAL_FAILURE(CheckFlatStagingOracle());
}

namespace {
TEST_F(RegionalWaterFixture, ContiguousPreflightRetainsEveryArrayCheckAndFirstDiagnostic) {
    Owner({0, 0});
    struct Fault {
        const char* diagnostic = nullptr;
        std::function<void(Chunk&)> corrupt{};
    };
    const auto infinity = std::numeric_limits<float>::infinity();
    const std::vector<Fault> faults{
        {"Regional water grid shape refused",
         [](Chunk& c) {
             c.water_depth_mm.pop_back();
         }},
        {"Regional water grid shape refused",
         [](Chunk& c) {
             c.water_bed_mm.pop_back();
         }},
        {"Regional water grid shape refused",
         [](Chunk& c) {
             c.water_edge_flux.pop_back();
         }},
        {"Regional water grid shape refused",
         [](Chunk& c) {
             c.water_level_data.pop_back();
         }},
        {"Regional water grid shape refused",
         [](Chunk& c) {
             c.water_flow_data.pop_back();
         }},
        {"Regional water grid shape refused",
         [](Chunk& c) {
             c.water_sim_terrain_height.pop_back();
         }},
        {"Regional water grid shape refused",
         [](Chunk& c) {
             c.water_rest_level.resize(1);
         }},
        {"Regional water cell refused",
         [](Chunk& c) {
             c.water_depth_mm[63] = -1;
         }},
        {"Regional water cell refused",
         [infinity](Chunk& c) {
             c.water_level_data[63] = infinity;
         }},
        {"Regional water cell refused",
         [infinity](Chunk& c) {
             c.water_flow_data[63].x = infinity;
         }},
        {"Regional water cell refused",
         [infinity](Chunk& c) {
             c.water_flow_data[63].y = infinity;
         }},
        {"Regional water cell refused",
         [infinity](Chunk& c) {
             c.water_sim_terrain_height[63] = infinity;
         }},
        {"Regional water continuation refused",
         [](Chunk& c) {
             c.ticks_below_threshold = -1;
         }},
        {"Regional water continuation refused",
         [](Chunk& c) {
             c.water_mesh_dirty_ticks = -1;
         }},
        {"Regional water continuation refused",
         [](Chunk& c) {
             c.water_mesh_dirty_ticks = 16;
         }},
        {"Regional water continuation refused",
         [infinity](Chunk& c) {
             c.max_water_delta_last_tick = infinity;
         }},
        {"Regional water continuation refused",
         [](Chunk& c) {
             c.max_water_delta_last_tick = -1.0f;
         }},
        {"Regional water resolution refused",
         [](Chunk& c) {
             c.current_water_resolution.store(3);
         }},
        {"Regional water uninitialized storage refused",
         [](Chunk& c) {
             c.has_water_sim.store(false);
         }},
        // Simultaneous defects preserve shape -> continuation -> cell precedence.
        {"Regional water grid shape refused",
         [](Chunk& c) {
             c.water_bed_mm.clear();
             c.ticks_below_threshold = -1;
             c.water_depth_mm[0] = -1;
         }},
        {"Regional water continuation refused",
         [](Chunk& c) {
             c.ticks_below_threshold = -1;
             c.water_depth_mm[0] = -1;
         }},
    };
    for (std::size_t i = 0; i < faults.size(); ++i) {
        SCOPED_TRACE(i);
        chunks = {WaterChunk({0, 0, 0})};
        chunks[0]->is_water_sleeping.store(true);
        Systems::RegionalWater::PlanningCache cache;
        ASSERT_NO_THROW(cache.Refresh(chunks, ledger, 8)); // Warm, off-due and sleeping.
        faults[i].corrupt(*chunks[0]);
        const auto before = CaptureWater(*chunks[0]);
        try {
            cache.Refresh(chunks, ledger, 8);
            FAIL() << "preflight accepted corrupt water";
        } catch (const std::runtime_error& error) {
            EXPECT_STREQ(error.what(), faults[i].diagnostic);
        }
        EXPECT_EQ(CaptureWater(*chunks[0]), before);
    }
}

TEST_F(RegionalWaterFixture, ResidentViewRefreshSeesReplacementOwnershipAndLoadingBeforePlanning) {
    Owner({0, 0});
    ResidentChunkRoster roster;
    ResidentChunkRoster::Map resident;
    auto first = WaterChunk({0, 0, 0});
    const auto id = first->get_id();
    resident.emplace(id, first);
    Systems::RegionalWater::PlanningCache cache;
    auto view = roster.refresh(resident);
    ASSERT_EQ(view.size(), 1u);
    cache.Refresh(view, ledger, 8);
    const auto* storage = view.data();
    const auto owners = first.use_count();
    view = roster.refresh(resident);
    EXPECT_EQ(view.data(), storage);
    EXPECT_EQ(first.use_count(), owners); // No per-tick shared-owner acquisition.

    auto replacement = WaterChunk({0, 0, 0}, 333);
    resident.at(id) = replacement;
    view = roster.refresh(resident);
    cache.Refresh(view, ledger, 8);
    ASSERT_NE(cache.Find(id), nullptr);
    EXPECT_EQ(cache.Find(id)->chunk, replacement.get());
    EXPECT_EQ(view[0], replacement);

    // Same address with distinct shared ownership must also refresh the roster.
    // replacement retains the actual lifetime while this no-op owner is installed.
    const std::shared_ptr<Chunk> alternate_owner(replacement.get(), [](Chunk*) {});
    resident.at(id) = alternate_owner;
    view = roster.refresh(resident);
    EXPECT_FALSE(view[0].owner_before(alternate_owner));
    EXPECT_FALSE(alternate_owner.owner_before(view[0]));
    cache.Refresh(view, ledger, 8);
    ASSERT_NE(cache.Find(id), nullptr);
    EXPECT_FALSE(cache.Find(id)->identity.owner_before(alternate_owner));
    EXPECT_FALSE(alternate_owner.owner_before(cache.Find(id)->identity));

    replacement->set_state(ChunkState::Loading);
    view = roster.refresh(resident);
    cache.Refresh(view, ledger, 8);
    EXPECT_EQ(cache.Find(id), nullptr);
    replacement->set_state(ChunkState::Idle);
    view = roster.refresh(resident);
    cache.Refresh(view, ledger, 8);
    ASSERT_NE(cache.Find(id), nullptr);
    EXPECT_EQ(cache.Find(id)->chunk, replacement.get());
    resident.clear();
    view = roster.refresh(resident);
    EXPECT_TRUE(view.empty());
    cache.Refresh(view, ledger, 8);
    EXPECT_TRUE(cache.entries.empty());
}

TEST_F(RegionalWaterFixture, UnloadReleasesSnapshotAndRosterOwnersWithoutAnotherQuery) {
    shield.debug_set_streaming_radius_cap(1);
    for (const bool warm_roster : {false, true}) {
        SCOPED_TRACE(warm_roster);
        auto chunk = std::make_shared<Chunk>(IVec3(1000, 0, 1000));
        chunk->set_state(ChunkState::Loading);
        ASSERT_TRUE(shield.adopt_streamed_chunk(chunk));
        ASSERT_EQ(chunk.use_count(), 2); // Local owner and streaming map only.
        {
            const auto snapshot = shield.snapshot_streamed_chunks();
            ASSERT_EQ(snapshot.size(), 1u);
            EXPECT_EQ(chunk.use_count(), 3);
        }
        EXPECT_EQ(chunk.use_count(), 2); // Ordinary snapshots leave no roster owner.
        if (warm_roster) {
            ASSERT_EQ(shield.resident_streamed_chunks().size(), 1u);
            EXPECT_EQ(chunk.use_count(), 3);
        }
        const std::weak_ptr<Chunk> lifetime = chunk;
        chunk.reset();
        // One complete activation interval; no jobs or water solver own this chunk.
        for (int frame = 0; frame < 4; ++frame)
            shield.update(registry, Vec3(0), nullptr);
        EXPECT_EQ(shield.get_last_streaming_budget_stats().unloaded_chunks, 1u);
        EXPECT_TRUE(lifetime.expired()); // No subsequent snapshot or roster refresh.
        shield.clear_world(nullptr);
    }
}

TEST_F(RegionalWaterFixture, ClearWorldReleasesRosterOwnersWithoutAnotherQuery) {
    auto chunk = WaterChunk({0, 0, 0});
    ASSERT_TRUE(shield.adopt_streamed_chunk(chunk));
    ASSERT_EQ(shield.resident_streamed_chunks().size(), 1u);
    ASSERT_EQ(chunk.use_count(), 3);
    const std::weak_ptr<Chunk> lifetime = chunk;
    chunk.reset();
    shield.clear_world(nullptr);
    EXPECT_TRUE(lifetime.expired()); // No subsequent snapshot or roster refresh.
}

TEST_F(RegionalWaterFixture, WorldResidentViewSharesSnapshotOrderAndObservesLoading) {
    Owner({0, 0});
    auto chunk = WaterChunk({0, 0, 0});
    ASSERT_TRUE(shield.adopt_streamed_chunk(chunk));
    const auto snapshot = shield.snapshot_streamed_chunks();
    auto view = shield.resident_streamed_chunks();
    ASSERT_EQ(view.size(), snapshot.size());
    EXPECT_TRUE(std::equal(view.begin(), view.end(), snapshot.begin()));
    const auto* storage = view.data();
    view = shield.resident_streamed_chunks();
    EXPECT_EQ(view.data(), storage);
    Systems::RegionalWater::PlanningCache cache;
    cache.Refresh(view, ledger, 8);
    ASSERT_NE(cache.Find(chunk->get_id()), nullptr);
    chunk->set_state(ChunkState::Loading);
    cache.Refresh(shield.resident_streamed_chunks(), ledger, 8);
    EXPECT_EQ(cache.Find(chunk->get_id()), nullptr);
    chunk->set_state(ChunkState::Idle);
    cache.Refresh(shield.resident_streamed_chunks(), ledger, 8);
    ASSERT_NE(cache.Find(chunk->get_id()), nullptr);
}
TEST_F(RegionalWaterFixture, UnchangedAuthorityPublicationKeepsExistingRegionRevisions) {
    auto promoted = WaterChunk({0, 0, 0});
    std::vector<Chunk*> participants{promoted.get()};
    {
        auto publication = shield.prepare_water_authority_publication(participants);
        EXPECT_TRUE(publication.changed);
        shield.publish_water_authority(std::move(publication));
    }
    promoted->mark_sdf_loaded_or_edited();
    const auto revision = shield.far_lod_authority_revision();
    const auto region = shield.far_lod_region_authority_revision(0, 0);
    {
        auto publication = shield.prepare_water_authority_publication(participants);
        EXPECT_FALSE(publication.changed);
        EXPECT_TRUE(publication.revisions.empty());
        shield.publish_water_authority(std::move(publication));
    }
    EXPECT_EQ(shield.far_lod_authority_revision(), revision);
    EXPECT_EQ(shield.far_lod_region_authority_revision(0, 0), region);
    EXPECT_GT(region, 0u);
}

TEST(RegionalWater, ResetWorkScratchRetainsStorageAndClearsAllOutputs) {
    Systems::WaterSystem::RegionalWork work;
    work.grants.reserve(8);
    work.visits.reserve(8);
    work.grants.push_back({});
    work.visits.push_back(123);
    work.total_cells = 64;
    work.mass_ok = false;
    const auto* grants = work.grants.data();
    const auto* visits = work.visits.data();
    work.Reset();
    EXPECT_EQ(work, Systems::WaterSystem::RegionalWork{});
    EXPECT_EQ(work.grants.data(), grants);
    EXPECT_EQ(work.visits.data(), visits);
}

} // namespace
