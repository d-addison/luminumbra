// Durable residency (S3): with set_durable_residency(true), an edited (LoadedOrEdited)
// chunk that streams out of the radius is PARKED as normalized sim truth instead of
// being erased, and returns byte-identically when the anchor comes back. Pristine
// (regenerable) chunks are never parked. A parked-bytes budget that is too small keeps
// dirty chunks resident and requests a persistence save. Also pins the chunk-level
// contracts the parking path relies on (normalize_for_park, the water revision and the
// water-authority promotion).
#include "../support/FarEditedCacheFixture.h"
#include "../support/SimulationContentFixture.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/core/WaterComponents.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WaterSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/GameSession.h"

namespace fs = std::filesystem;

namespace {

using Luminumbra::Chunk;
using Luminumbra::ChunkSdfProvenance;
using Luminumbra::ChunkState;
using Luminumbra::IVec3;
using Luminumbra::JobSystem;
using Luminumbra::Vec3;
using Luminumbra::Systems::SHIELD_WorldSystem;
using Luminumbra::world::GameSession;

// FNV-1a-64 over a raw byte span (the meshing_hardening_test helper).
constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

std::uint64_t Fnv1a64(const void* data, std::size_t size) {
    std::uint64_t hash = kFnvOffsetBasis;
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= static_cast<std::uint64_t>(bytes[i]);
        hash *= kFnvPrime;
    }
    return hash;
}

template<typename T>
std::uint64_t HashVec(const std::vector<T>& v) {
    return Fnv1a64(v.data(), v.size() * sizeof(T));
}

// Real generated terrain, but no unrelated caves, structures, biome relief or erosion bakes.
// A dry plane at Y=8 puts the radius-3 spawn edit wholly inside one chunk (spawn X/Z=8),
// above sea level and away from chunk seams. The SDF, mesh, collision and save paths still run.
constexpr char kResidencyPreset[] = R"({
    "schema_rev": 6,
    "generation_params": {
        "terrain": {
            "base_frequency": 0.01,
            "base_amplitude": 0.0,
            "octaves": 1,
            "persistence": 0.5,
            "lacunarity": 2.0,
            "height_offset": 8.0,
            "shaping": {
                "peaks_amplitude": 0.0,
                "domain_warp_amplitude": 0.0,
                "continental_spline": [[-1.0, 0.0], [1.0, 0.0]]
            },
            "hydro": {"enabled": false}
        },
        "features": {
            "caves_enabled": false,
            "cave_frequency": 0.02
        }
    }
})";

// Temp root with the test preset and required simulation species; no client assets.
// The root is unique per fixture instance so concurrent common_tests processes
// never share (or clobber) a directory. The destructor removes the tree.
class HeadlessRoot {
public:
    HeadlessRoot() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = fs::temp_directory_path() /
                ("luminumbra_durable_residency_test_" + std::to_string(stamp));
        fs::create_directories(root_ / "worlds" / "atlas" / "presets");
        std::ofstream preset;
        preset.exceptions(std::ios::failbit | std::ios::badbit);
        preset.open(root_ / "worlds" / "atlas" / "presets" / "durable.json");
        preset << kResidencyPreset;
        preset.close();
        Luminumbra::TestSupport::WriteSimulationContentFixture(root_);
    }
    ~HeadlessRoot() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }
    [[nodiscard]] std::string root_string() const {
        return root_.string() + static_cast<char>(fs::path::preferred_separator);
    }

private:
    fs::path root_;
};

// A radius-1 disc streams five columns, each with the surface chunk and its two
// vertical neighbours. Editing also builds the production radius-2 square (75 chunks);
// the far anchor still evicts that entire band. The SDF edit touches one chunk;
// its coupled water-bed edit can make the other two chunks in that column authoritative too.
constexpr int kStreamRadiusCap = 1;
// Far anchor distance in chunks along X: well outside radius + 2 of the spawn anchor.
constexpr float kFarChunks = 40.0f;
// Activation runs every four updates. Three passes suffice for this small set:
// generate/restore, mesh and drain, then retry any eviction deferred by in-flight work.
// Each update explicitly drains the real streaming jobs, independent of worker timing.
constexpr int kSettleIterations = 12;
constexpr int kReturnIterations = kSettleIterations;

// One headless world per TEST. Member order: root outlives the session, the job
// system outlives the session, and the destructor tears the session down before
// shutting the job system down.
class DurableFixture {
public:
    explicit DurableFixture(bool durable, bool active_regions = false) {
        // CTest runs cases in separate processes; bound each small fixture's worker pool.
        jobs.startup(2);
        session = std::make_unique<GameSession>();
        session->SetActiveRegionsEnabled(active_regions);
        // This fixture needs regenerable dry neighbours. Rain is water divergence
        // and correctly establishes durable authority even without a voxel edit.
        session->SetWeatherRainEnabled(false);
        session->SetJobSystem(&jobs);
        session->SetRootPath(root.root_string());
        created = session->CreateWorld("DurableResidency", "12345", "durable");
        if (!created) {
            error = "CreateWorld failed";
            return;
        }
        world = session->GetWorldSystem();
        if (world == nullptr) {
            created = false;
            error = "no world system";
            return;
        }
        world->debug_set_streaming_radius_cap(kStreamRadiusCap);
        if (!active_regions) {
            world->set_durable_residency(durable);
        }
        physics = session->GetPhysicsSystem();
        spawn = session->GetMetadata().spawnPoint;
    }
    ~DurableFixture() {
        session.reset();
        jobs.shutdown();
    }
    DurableFixture(const DurableFixture&) = delete;
    DurableFixture& operator=(const DurableFixture&) = delete;

    Vec3 AnchorAt(float x, float z) {
        return Vec3(x, world->GetTerrainHeightAt(x, z) + 2.0f, z);
    }
    Vec3 SpawnAnchor() {
        return AnchorAt(spawn.x, spawn.z);
    }
    Vec3 FarAnchor() {
        return AnchorAt(spawn.x + kFarChunks * static_cast<float>(Luminumbra::CHUNK_SIZE_X),
                        spawn.z);
    }
    Vec3 SpawnSurfacePoint() {
        return Vec3(spawn.x, world->GetTerrainHeightAt(spawn.x, spawn.z), spawn.z);
    }

    void Settle(const Vec3& anchor, int iterations) {
        for (int i = 0; i < iterations; ++i) {
            world->update(session->GetRegistry(), {anchor}, physics);
            world->wait_for_streaming_jobs();
        }
    }

    // First resident chunk whose SDF is an edited / loaded authority.
    std::shared_ptr<Chunk> ResidentLoadedChunk() {
        for (const auto& chunk : world->snapshot_streamed_chunks()) {
            if (chunk && chunk->sdf_provenance() == ChunkSdfProvenance::LoadedOrEdited)
                return chunk;
        }
        return nullptr;
    }

    // Spawn-surface edit, then the first resident LoadedOrEdited chunk (or null).
    std::shared_ptr<Chunk> EditAtSpawnAndFindEdited() {
        const int modified = world->EditTerrainVoxel(SpawnSurfacePoint(), 3.0f, false, physics);
        if (modified <= 0)
            return nullptr;
        return ResidentLoadedChunk();
    }

    HeadlessRoot root;
    JobSystem jobs;
    std::unique_ptr<GameSession> session;
    SHIELD_WorldSystem* world = nullptr;
    Luminumbra::Systems::PhysicsSystem* physics = nullptr;
    Vec3 spawn{};
    bool created = false;
    std::string error;
};

bool Contains(const std::vector<std::shared_ptr<Chunk>>& chunks, const Chunk* target) {
    return std::any_of(chunks.begin(), chunks.end(), [target](const auto& chunk) {
        return chunk.get() == target;
    });
}

TEST(WorldDurableResidency, FlagOffEvictionStillErases) {
    DurableFixture fx(/*durable=*/false);
    ASSERT_TRUE(fx.created) << fx.error;
    fx.Settle(fx.SpawnAnchor(), kSettleIterations);

    const int modified =
        fx.world->EditTerrainVoxel(fx.SpawnSurfacePoint(), 3.0f, false, fx.physics);
    ASSERT_GT(modified, 0);
    const auto edited = fx.ResidentLoadedChunk();
    ASSERT_TRUE(edited);
    const IVec3 coords = edited->get_coords();

    fx.Settle(fx.FarAnchor(), kSettleIterations);
    EXPECT_FALSE(fx.world->find_streamed_chunk(coords));
    EXPECT_FALSE(fx.world->find_authoritative_chunk(coords));
    EXPECT_EQ(fx.world->parking_status().parked_count, 0u);
}

TEST(WorldDurableResidency, EditedChunkParksAndReturnsIdentically) {
    DurableFixture fx(/*durable=*/true);
    ASSERT_TRUE(fx.created) << fx.error;
    fx.Settle(fx.SpawnAnchor(), kSettleIterations);

    const auto edited = fx.EditAtSpawnAndFindEdited();
    ASSERT_TRUE(edited);
    const IVec3 coords = edited->get_coords();
    const std::uint64_t recorded_hash = HashVec(edited->sdf_data);

    fx.world->note_persistence_tick(7);
    fx.Settle(fx.FarAnchor(), kSettleIterations);
    EXPECT_FALSE(fx.world->find_streamed_chunk(coords));
    EXPECT_EQ(fx.world->find_authoritative_chunk(coords), edited);

    const auto status = fx.world->parking_status();
    EXPECT_GE(status.parked_count, 1u);
    EXPECT_GT(status.parked_bytes, 0u);
    EXPECT_EQ(status.oldest_parked_tick, 7);

    const auto parked_snapshot = fx.world->snapshot_parked_chunks();
    EXPECT_TRUE(Contains(parked_snapshot, edited.get()));
    for (const auto& parked : parked_snapshot) {
        ASSERT_TRUE(parked);
        EXPECT_TRUE(parked->needs_authority());
        EXPECT_FALSE(parked->has_collision.load());
        EXPECT_EQ(parked->current_lod.load(), -1);
    }

    fx.Settle(fx.SpawnAnchor(), kReturnIterations);
    EXPECT_EQ(fx.world->find_streamed_chunk(coords), edited);
    EXPECT_FALSE(Contains(fx.world->snapshot_parked_chunks(), edited.get()));
    EXPECT_EQ(HashVec(edited->sdf_data), recorded_hash);
}

TEST(WorldDurableResidency, WholeSnapshotCommitReleasesParkedPayloadAndReturnRestoresTruth) {
    DurableFixture fx(/*durable=*/true, /*active_regions=*/true);
    ASSERT_TRUE(fx.created) << fx.error;
    fx.Settle(fx.SpawnAnchor(), kSettleIterations);
    const auto edited = fx.EditAtSpawnAndFindEdited();
    ASSERT_NE(edited, nullptr);
    const auto coords = edited->get_coords();
    const auto sdf_hash = HashVec(edited->sdf_data);
    fx.Settle(fx.FarAnchor(), kSettleIterations);
    ASSERT_TRUE(Contains(fx.world->snapshot_parked_chunks(), edited.get()));
    const auto before = fx.session->ComputeChunkStateHash();
    const auto parked_bytes = fx.world->parking_status().parked_bytes;
    ASSERT_TRUE(fx.session->SaveWorldState());
    EXPECT_FALSE(Contains(fx.world->snapshot_parked_chunks(), edited.get()));
    EXPECT_LT(fx.world->parking_status().parked_bytes, parked_bytes);
    EXPECT_EQ(fx.session->ComputeChunkStateHash(), before);
    EXPECT_FALSE(fx.world->find_streamed_chunk(coords));
    ASSERT_TRUE(fx.world->restore_authoritative_chunk(coords));
    EXPECT_FALSE(fx.world->durable_read_failed());
    EXPECT_FALSE(fx.session->GetRegionSchedulerFault());
    const auto restored = fx.world->find_streamed_chunk(coords);
    ASSERT_NE(restored, nullptr);
    EXPECT_NE(restored, edited);
    EXPECT_EQ(HashVec(restored->sdf_data), sdf_hash);
    EXPECT_EQ(restored->sdf_provenance(), ChunkSdfProvenance::LoadedOrEdited);
}

TEST(WorldDurableResidency, FailedWholeSaveRetainsDirtyParkedPayloadAndInventory) {
    DurableFixture fx(/*durable=*/true, /*active_regions=*/true);
    ASSERT_TRUE(fx.created) << fx.error;
    fx.Settle(fx.SpawnAnchor(), kSettleIterations);
    const auto edited = fx.EditAtSpawnAndFindEdited();
    ASSERT_NE(edited, nullptr);
    fx.Settle(fx.FarAnchor(), kSettleIterations);
    ASSERT_TRUE(Contains(fx.world->snapshot_parked_chunks(), edited.get()));
    const auto inventory = fx.world->durable_chunk_inventory();
    Luminumbra::Persistence::WorldSaveService::set_before_active_regions_replace_for_testing(
        [] { return false; });
    const bool saved = fx.session->SaveWorldState();
    Luminumbra::Persistence::WorldSaveService::set_before_active_regions_replace_for_testing(
        nullptr);
    EXPECT_FALSE(saved);
    EXPECT_TRUE(edited->is_voxel_data_dirty());
    EXPECT_TRUE(Contains(fx.world->snapshot_parked_chunks(), edited.get()));
    EXPECT_EQ(fx.world->durable_chunk_inventory(), inventory);
}

TEST(WorldDurableResidency, OnlyAuthoritativeChunksAreParked) {
    DurableFixture fx(/*durable=*/true);
    ASSERT_TRUE(fx.created) << fx.error;
    fx.Settle(fx.SpawnAnchor(), kSettleIterations);
    const auto pristine = fx.world->snapshot_streamed_chunks();
    ASSERT_FALSE(pristine.empty());
    for (const auto& chunk : pristine) {
        ASSERT_TRUE(chunk);
        ASSERT_FALSE(chunk->needs_authority());
    }
    // Dry terrain has no naturally moving water; include an actual edit so the
    // authoritative half of this contract cannot pass with an empty parked set.
    const auto edited = fx.EditAtSpawnAndFindEdited();
    ASSERT_TRUE(edited);
    std::vector<std::shared_ptr<Chunk>> regenerable;
    for (const auto& chunk : pristine) {
        if (!chunk->needs_authority())
            regenerable.push_back(chunk);
    }
    ASSERT_FALSE(regenerable.empty());
    fx.Settle(fx.FarAnchor(), kSettleIterations);
    EXPECT_TRUE(Contains(fx.world->snapshot_parked_chunks(), edited.get()));
    for (const auto& chunk : regenerable)
        EXPECT_FALSE(fx.world->find_authoritative_chunk(chunk->get_coords()));
    // Water that moved makes a chunk authoritative; everything else is regenerable and erased.
    for (const auto& parked : fx.world->snapshot_parked_chunks()) {
        ASSERT_TRUE(parked);
        EXPECT_TRUE(parked->needs_authority());
    }
}

TEST(WorldDurableResidency, ByteLimitKeepsDirtyChunkResidentAndRequestsSave) {
    DurableFixture fx(/*durable=*/true);
    ASSERT_TRUE(fx.created) << fx.error;
    fx.world->set_parked_bytes_limit(1);
    fx.Settle(fx.SpawnAnchor(), kSettleIterations);

    const auto edited = fx.EditAtSpawnAndFindEdited();
    ASSERT_TRUE(edited);
    const IVec3 coords = edited->get_coords();

    fx.Settle(fx.FarAnchor(), kSettleIterations);
    EXPECT_TRUE(fx.world->find_streamed_chunk(coords));
    const auto status = fx.world->parking_status();
    EXPECT_EQ(status.parked_count, 0u);
    EXPECT_GE(status.blocked_dirty, 1u);
    EXPECT_TRUE(status.save_requested);
    EXPECT_TRUE(fx.world->persistence_save_requested());

    fx.world->clear_persistence_save_request();
    EXPECT_FALSE(fx.world->persistence_save_requested());
}

TEST(WorldDurableResidency, EditOfParkedChunkUnparksIt) {
    DurableFixture fx(/*durable=*/true);
    ASSERT_TRUE(fx.created) << fx.error;
    fx.Settle(fx.SpawnAnchor(), kSettleIterations);

    const auto edited = fx.EditAtSpawnAndFindEdited();
    ASSERT_TRUE(edited);
    const IVec3 coords = edited->get_coords();

    fx.Settle(fx.FarAnchor(), kSettleIterations);
    ASSERT_TRUE(fx.world->find_authoritative_chunk(coords));
    ASSERT_FALSE(fx.world->find_streamed_chunk(coords));
    ASSERT_GE(fx.world->parking_status().parked_count, 1u);

    const float cs_x = static_cast<float>(Luminumbra::CHUNK_SIZE_X);
    const float cs_y = static_cast<float>(Luminumbra::CHUNK_SIZE_Y);
    const float cs_z = static_cast<float>(Luminumbra::CHUNK_SIZE_Z);
    const Vec3 center(static_cast<float>(coords.x) * cs_x + 8.0f,
                      static_cast<float>(coords.y) * cs_y + 8.0f,
                      static_cast<float>(coords.z) * cs_z + 8.0f);
    // Whether or not this edit changes voxels, touching a parked chunk unparks it.
    static_cast<void>(fx.world->EditTerrainVoxel(center, 2.0f, false, fx.physics));

    EXPECT_TRUE(fx.world->find_streamed_chunk(coords));
    EXPECT_FALSE(Contains(fx.world->snapshot_parked_chunks(), edited.get()));
}

TEST(WorldDurableResidency, AdoptReplacesParkedEntry) {
    DurableFixture fx(/*durable=*/true);
    ASSERT_TRUE(fx.created) << fx.error;
    fx.Settle(fx.SpawnAnchor(), kSettleIterations);

    const auto edited = fx.EditAtSpawnAndFindEdited();
    ASSERT_TRUE(edited);
    const IVec3 coords = edited->get_coords();

    fx.Settle(fx.FarAnchor(), kSettleIterations);
    const auto parked = fx.world->find_authoritative_chunk(coords);
    ASSERT_TRUE(parked);
    ASSERT_FALSE(fx.world->find_streamed_chunk(coords));
    const std::size_t before = fx.world->parking_status().parked_count;
    ASSERT_GE(before, 1u);

    auto fresh = std::make_shared<Chunk>(coords);
    fresh->set_state(ChunkState::Idle);
    fresh->sdf_data = parked->sdf_data;

    EXPECT_TRUE(fx.world->adopt_streamed_chunk(fresh));
    EXPECT_EQ(fx.world->parking_status().parked_count, before - 1);
    EXPECT_EQ(fx.world->find_authoritative_chunk(coords), fresh);
}

TEST(WorldDurableResidency, ParkedChunksAppearOnceInAuthoritativeSnapshot) {
    DurableFixture fx(/*durable=*/true);
    ASSERT_TRUE(fx.created) << fx.error;
    fx.Settle(fx.SpawnAnchor(), kSettleIterations);

    const auto edited = fx.EditAtSpawnAndFindEdited();
    ASSERT_TRUE(edited);

    fx.Settle(fx.FarAnchor(), kSettleIterations);
    const auto snapshot = fx.world->snapshot_authoritative_chunks();

    using IdT = decltype(std::declval<const Chunk&>().get_id());
    std::set<const Chunk*> pointers;
    std::set<IdT> ids;
    std::size_t entries = 0;
    for (const auto& chunk : snapshot) {
        ASSERT_TRUE(chunk);
        ++entries;
        pointers.insert(chunk.get());
        ids.insert(chunk->get_id());
    }
    EXPECT_EQ(pointers.size(), entries) << "duplicate chunk pointer in authoritative snapshot";
    EXPECT_EQ(ids.size(), entries) << "duplicate chunk id in authoritative snapshot";
    EXPECT_TRUE(Contains(snapshot, edited.get()));
}

TEST(WorldDurableResidency, SyncWaterAuthorityPromotesWaterDirtyChunk) {
    DurableFixture fx(/*durable=*/true);
    ASSERT_TRUE(fx.created) << fx.error;
    fx.Settle(fx.SpawnAnchor(), kSettleIterations);

    std::shared_ptr<Chunk> generated;
    for (const auto& chunk : fx.world->snapshot_streamed_chunks()) {
        if (chunk && chunk->sdf_provenance() == ChunkSdfProvenance::GeneratedCurrentParams &&
            !chunk->needs_authority()) {
            generated = chunk;
            break;
        }
    }
    ASSERT_TRUE(generated);

    generated->mark_water_state_dirty();
    EXPECT_TRUE(generated->needs_authority());
    EXPECT_GE(generated->water_revision(), 1u);

    fx.world->sync_water_authority();
    EXPECT_EQ(generated->sdf_provenance(), ChunkSdfProvenance::LoadedOrEdited);
    EXPECT_TRUE(generated->has_water_authority());
    EXPECT_TRUE(generated->is_voxel_data_dirty());
    EXPECT_FALSE(generated->is_water_state_dirty());
    EXPECT_TRUE(generated->needs_authority());

    generated->mark_sdf_generated_current_params();
    EXPECT_EQ(generated->sdf_provenance(), ChunkSdfProvenance::LoadedOrEdited);
    EXPECT_GE(generated->voxel_revision(), 1u);
    // A backfill must not acknowledge the unsaved water obligation carried by the dirty bit.
    generated->clear_voxel_data_dirty_after_generation();
    EXPECT_TRUE(generated->is_voxel_data_dirty());
}

TEST(WorldDurableResidency, ChunkNormalizeForParkDropsRenderBuffers) {
    Chunk chunk(IVec3(0, 0, 0));
    chunk.mesh_vertices.push_back(Luminumbra::VoxelVertex{});
    chunk.mesh_indices.push_back(0u);
    chunk.has_collision.store(true);
    chunk.current_lod.store(2);
    chunk.sdf_data.assign(10, 1.0f);

    chunk.normalize_for_park();

    EXPECT_TRUE(chunk.mesh_vertices.empty());
    EXPECT_EQ(chunk.mesh_vertices.capacity(), 0u);
    EXPECT_FALSE(chunk.has_collision.load());
    EXPECT_EQ(chunk.current_lod.load(), -1);
    EXPECT_EQ(chunk.get_state(), ChunkState::Idle);
    EXPECT_EQ(chunk.sdf_data.size(), 10u);
    EXPECT_GE(chunk.estimated_sim_bytes(), 10 * sizeof(float));
    EXPECT_GE(chunk.estimated_heap_bytes(), chunk.estimated_sim_bytes());
}

TEST(WorldDurableResidency, ChunkWaterRevisionMonotonic) {
    Chunk chunk(IVec3(0, 0, 0));
    chunk.mark_water_state_dirty();
    chunk.mark_water_state_dirty();
    EXPECT_EQ(chunk.water_revision(), 2u);

    chunk.clear_water_state_dirty();
    EXPECT_EQ(chunk.water_revision(), 2u);
    EXPECT_FALSE(chunk.is_water_state_dirty());
}

TEST(WorldDurableResidency, SessionSavesAndReloadsParkedEditedChunk) {
    DurableFixture fx(/*durable=*/true, /*active_regions=*/true);
    ASSERT_TRUE(fx.created) << fx.error;
    ASSERT_TRUE(fx.world->durable_residency());
    fx.Settle(fx.SpawnAnchor(), kSettleIterations);

    const auto edited = fx.EditAtSpawnAndFindEdited();
    ASSERT_TRUE(edited);
    const IVec3 coords = edited->get_coords();
    const std::uint64_t recorded_hash = HashVec(edited->sdf_data);

    fx.Settle(fx.FarAnchor(), kSettleIterations);
    ASSERT_FALSE(fx.world->find_streamed_chunk(coords));
    ASSERT_EQ(fx.world->find_authoritative_chunk(coords), edited);

    ASSERT_TRUE(fx.session->SaveWorldState());
    EXPECT_FALSE(edited->is_voxel_data_dirty());

    GameSession reloaded;
    reloaded.SetJobSystem(&fx.jobs);
    reloaded.SetRootPath(fx.root.root_string());
    reloaded.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(reloaded.LoadWorld(fx.session->GetMetadata().worldId));
    ASSERT_NE(reloaded.GetWorldSystem(), nullptr);
    const auto loaded = reloaded.GetWorldSystem()->find_streamed_chunk(coords);
    ASSERT_TRUE(loaded);
    EXPECT_EQ(HashVec(loaded->sdf_data), recorded_hash);
}

TEST(WorldDurableResidency, FailedSessionSaveRearmsDirtyChunkForRetry) {
    DurableFixture fx(/*durable=*/true, /*active_regions=*/true);
    ASSERT_TRUE(fx.created) << fx.error;
    fx.Settle(fx.SpawnAnchor(), kSettleIterations);

    const auto edited = fx.EditAtSpawnAndFindEdited();
    ASSERT_TRUE(edited);
    ASSERT_TRUE(edited->is_voxel_data_dirty());

    Luminumbra::Persistence::WorldSaveService::set_before_active_regions_replace_for_testing(
        [] { return false; });
    const bool failed_save = fx.session->SaveWorldState();
    Luminumbra::Persistence::WorldSaveService::set_before_active_regions_replace_for_testing(
        nullptr);
    EXPECT_FALSE(failed_save);
    EXPECT_TRUE(edited->is_voxel_data_dirty());
    EXPECT_EQ(fx.session->GetPersistenceStatus().failures, 1u);

    // An explicit retry rewrites the re-armed chunk and acknowledges it.
    EXPECT_TRUE(fx.session->SaveWorldState());
    EXPECT_FALSE(edited->is_voxel_data_dirty());
}

TEST(WorldDurableResidency, NonQuiescentChunkIsNotParked) {
    DurableFixture fx(/*durable=*/true);
    ASSERT_TRUE(fx.created) << fx.error;
    fx.Settle(fx.SpawnAnchor(), kSettleIterations);

    const auto edited = fx.EditAtSpawnAndFindEdited();
    ASSERT_TRUE(edited);
    const IVec3 coords = edited->get_coords();

    // A completed mesh-result marker is consumed by update(), so it cannot hold a
    // chunk non-quiescent across a settle loop. An unfinished generation state can.
    edited->set_state(Luminumbra::ChunkState::Loading);
    fx.Settle(fx.FarAnchor(), kSettleIterations);
    EXPECT_EQ(fx.world->find_streamed_chunk(coords), edited);
    const auto parked = fx.world->snapshot_parked_chunks();
    EXPECT_EQ(std::find(parked.begin(), parked.end(), edited), parked.end());
    edited->set_state(Luminumbra::ChunkState::Idle);
}

TEST(WorldDurableResidency, FarCacheDefersNotificationsAcknowledgementAndParkedReleaseUntilCommit) {
    DurableFixture fx(/*durable=*/true, /*active_regions=*/true);
    ASSERT_TRUE(fx.created) << fx.error;
    fx.Settle(fx.SpawnAnchor(), kSettleIterations);
    const auto edited = fx.EditAtSpawnAndFindEdited();
    ASSERT_TRUE(edited);
    fx.Settle(fx.FarAnchor(), kSettleIterations);
    ASSERT_TRUE(Contains(fx.world->snapshot_parked_chunks(), edited.get()));
    ASSERT_TRUE(edited->is_voxel_data_dirty());
    auto policy = Luminumbra::TestSupport::FarCachePolicy();
    // Exercise a successful authoritative commit with optional cache work still due.
    policy->limits.capture_source = {};
    auto reports = std::make_shared<std::vector<Luminumbra::World::FarCacheReport>>();
    policy->after_commit = [reports](const auto& report) {
        reports->push_back(report);
    };
    fx.session->SetFarEditedCachePolicy(policy);
    const auto revision = fx.world->far_lod_authority_revision();
    const auto staged = [&] {
        const bool saved = fx.session->SaveWorldState();
        EXPECT_TRUE(saved);
        EXPECT_TRUE(edited->is_voxel_data_dirty());
        EXPECT_TRUE(Contains(fx.world->snapshot_parked_chunks(), edited.get()));
        EXPECT_EQ(fx.world->far_lod_authority_revision(), revision);
        EXPECT_TRUE(reports->empty());
        return saved;
    };
    EXPECT_FALSE(fx.session->RunInSnapshotTransaction([&] {
        staged();
        return false;
    }));
    EXPECT_TRUE(edited->is_voxel_data_dirty());
    EXPECT_TRUE(Contains(fx.world->snapshot_parked_chunks(), edited.get()));
    EXPECT_EQ(fx.world->far_lod_authority_revision(), revision);
    EXPECT_TRUE(reports->empty());
    ASSERT_TRUE(fx.session->RunInSnapshotTransaction(staged));
    EXPECT_FALSE(edited->is_voxel_data_dirty());
    EXPECT_FALSE(Contains(fx.world->snapshot_parked_chunks(), edited.get()));
    EXPECT_GT(fx.world->far_lod_authority_revision(), revision);
    ASSERT_EQ(reports->size(), 1u);
    EXPECT_TRUE(reports->back().authoritative_success);
    EXPECT_TRUE(reports->back().derived_due);
    fx.session->SetFarEditedCachePolicy({});
}

} // namespace

TEST(WorldDurableResidency, S7WaterExcludesParkedAuthorityEvenWhenOwnerAndSourceAreDue) {
    DurableFixture fx(/*durable=*/true, /*active_regions=*/true);
    ASSERT_TRUE(fx.created) << fx.error;
    fx.Settle(fx.SpawnAnchor(), kSettleIterations);
    const auto edited = fx.EditAtSpawnAndFindEdited();
    ASSERT_NE(edited, nullptr);
    fx.Settle(fx.FarAnchor(), kSettleIterations);
    ASSERT_TRUE(Contains(fx.world->snapshot_parked_chunks(), edited.get()));
    edited->has_water_sim.store(true);
    edited->current_water_resolution.store(8);
    edited->water_depth_mm.assign(64, 1000);
    edited->water_bed_mm.assign(64, 1000);
    edited->water_edge_flux.assign(128, 100);
    edited->water_level_data.assign(64, 2.0f);
    edited->water_sim_terrain_height.assign(64, 1.0f);
    edited->water_flow_data.assign(64, Luminumbra::Vec2(0));
    edited->ticks_below_threshold = 7;
    const auto coords = edited->get_coords();
    const Vec3 position(static_cast<float>(coords.x * Luminumbra::CHUNK_SIZE_X) + 1.0f,
                        static_cast<float>(coords.y * Luminumbra::CHUNK_SIZE_Y) + 1.0f,
                        static_cast<float>(coords.z * Luminumbra::CHUNK_SIZE_Z) + 1.0f);
    using Luminumbra::world::ActiveRegionLedger;
    const auto owner = ActiveRegionLedger::region_at(position);
    ActiveRegionLedger ledger;
    ledger.mark_edited(owner, Luminumbra::world::WorldClock{});
    Luminumbra::world::RegionSchedule schedule;
    schedule.tick = ledger.tick();
    schedule.due = {owner};
    auto& registry = fx.session->GetRegistry();
    const auto entity = registry.create();
    registry.emplace<Luminumbra::Components::TransformComponent>(entity).position = position;
    registry.emplace<Luminumbra::Components::WaterFlowSourceComponent>(entity);
    const auto depth = edited->water_depth_mm, flux = edited->water_edge_flux;
    const auto revision = edited->water_revision();
    std::vector<Luminumbra::world::RegionWork> work;
    auto* water = fx.session->GetWaterSystem();
    water->SetHydrology(true, 2, 1);
    const auto resident = fx.world->snapshot_streamed_chunks();
    water->UpdateRegional(registry, resident, ledger, schedule, work);
    EXPECT_EQ(edited->water_depth_mm, depth);
    EXPECT_EQ(edited->water_edge_flux, flux);
    EXPECT_EQ(edited->water_revision(), revision);
    EXPECT_EQ(edited->ticks_below_threshold, 7);
    EXPECT_EQ(water->regional_work().used_windows, 0u);
    EXPECT_TRUE(work.empty());
    EXPECT_TRUE(Contains(fx.world->snapshot_parked_chunks(), edited.get()));
}
