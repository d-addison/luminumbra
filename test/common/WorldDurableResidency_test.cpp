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
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "luminumbra_common/ai/ScentField.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/core/WaterComponents.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WaterSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/FarDensityQuantization.h"
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
    const IVec3 coords = edited->get_coords();

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

namespace {
using Luminumbra::World::FarTileKey;
using Luminumbra::world::FarWorldPublication;
using Luminumbra::world::FarWorldPublicationRequest;

FarWorldPublicationRequest SavedPublicationRequest(std::span<const FarTileKey> keys) {
    FarWorldPublicationRequest request;
    request.world_epoch = 1;
    request.wanted = keys;
    request.limits = GameSession::FarWorldPublicationLimits();
    request.source_memory =
        std::make_shared<Luminumbra::World::FarVolumeMemoryPool>(2ull * 1024 * 1024 * 1024);
    for (auto& pool : request.memory)
        pool = std::make_shared<Luminumbra::World::FarVolumeMemoryPool>(256ull * 1024 * 1024);
    return request;
}

std::shared_ptr<Chunk> AdoptPublicationEdit(DurableFixture& fixture, IVec3 coordinates) {
    auto chunk = std::make_shared<Chunk>(coordinates);
    chunk->sdf_data.assign(17 * 17 * 17, -3.0f);
    chunk->material_data.assign(17 * 17 * 17, 7);
    chunk->mark_sdf_loaded_or_edited();
    chunk->mark_voxel_data_dirty();
    chunk->set_state(ChunkState::Idle);
    if (!fixture.world->adopt_streamed_chunk(chunk))
        return {};
    return chunk;
}
} // namespace

TEST(SavedFarWorldPublication, SuccessfulLoadPublishesAndFailedOpenKeepsCurrentBinding) {
    DurableFixture fx(false);
    ASSERT_TRUE(fx.created) << fx.error;
    fx.session->EnableFarWorldPersistenceSafety();
    ASSERT_TRUE(fx.session->SaveWorld());
    std::size_t replacements = 0;
    fx.session->SetBeforeWorldReplacement([&] { ++replacements; });
    const auto id = fx.session->GetMetadata().worldId;
    ASSERT_TRUE(fx.session->LoadWorld(id));
    EXPECT_EQ(replacements, 1u);
    const std::array keys{FarTileKey{5, 0, 0}};
    auto request = SavedPublicationRequest(keys);
    std::shared_ptr<const FarWorldPublication> publication;
    std::string diagnostic;
    ASSERT_TRUE(fx.session->CaptureFarWorldPublication(request, publication, diagnostic))
        << diagnostic;
    ASSERT_TRUE(publication);
    EXPECT_EQ(publication->save_root, fx.session->GetFarWorldSaveRoot());
    auto* const world = fx.session->GetWorldSystem();
    EXPECT_FALSE(fx.session->LoadWorld("missing_saved_world"));
    EXPECT_EQ(fx.session->GetWorldSystem(), world);
    EXPECT_EQ(replacements, 1u);
    EXPECT_EQ(fx.session->GetMetadata().worldId, id);
    const auto retained = publication;
    ASSERT_TRUE(fx.session->CaptureFarWorldPublication(request, publication, diagnostic))
        << diagnostic;
    EXPECT_EQ(publication->committed_generation, retained->committed_generation);
    EXPECT_GT(publication->publication_serial, retained->publication_serial);
    ASSERT_EQ(publication->tiles.size(), retained->tiles.size());
    EXPECT_EQ(publication->tiles[0].observation->content_identity(),
              retained->tiles[0].observation->content_identity());
}

TEST(SavedFarWorldPublication, DirtyParkedPreviewAndUnparkPreserveAuthorityAndFingerprint) {
    DurableFixture fx(false);
    ASSERT_TRUE(fx.created) << fx.error;
    fx.session->EnableFarWorldPersistenceSafety();
    EXPECT_FALSE(fx.session->ActiveRegionsEnabled());
    fx.Settle(fx.SpawnAnchor(), kSettleIterations);
    const std::array keys{FarTileKey{1, 0, 0}};
    auto request = SavedPublicationRequest(keys);
    std::shared_ptr<const FarWorldPublication> pristine, live, parked, unparked;
    std::string diagnostic;
    ASSERT_TRUE(fx.session->CaptureFarWorldPublication(request, pristine, diagnostic))
        << diagnostic;
    const auto edited = fx.EditAtSpawnAndFindEdited();
    ASSERT_TRUE(edited);
    fx.Settle(fx.SpawnAnchor(), kSettleIterations);
    ASSERT_TRUE(fx.session->CaptureFarWorldPublication(request, live, diagnostic)) << diagnostic;
    ASSERT_EQ(pristine->tiles.size(), 1u);
    ASSERT_EQ(live->tiles.size(), 1u);
    EXPECT_NE(live->tiles[0].observation->content_identity(),
              pristine->tiles[0].observation->content_identity());
    const auto density = edited->sdf_data;
    fx.Settle(fx.FarAnchor(), kSettleIterations);
    ASSERT_TRUE(Contains(fx.world->snapshot_parked_chunks(), edited.get()));
    ASSERT_TRUE(fx.session->CaptureFarWorldPublication(request, parked, diagnostic)) << diagnostic;
    EXPECT_TRUE(edited->is_voxel_data_dirty());
    EXPECT_EQ(edited->sdf_data, density);
    EXPECT_EQ(live->committed_generation, parked->committed_generation);
    ASSERT_EQ(live->tiles.size(), 1u);
    ASSERT_EQ(parked->tiles.size(), 1u);
    EXPECT_EQ(live->tiles[0].observation->content_identity(),
              parked->tiles[0].observation->content_identity());
    ASSERT_TRUE(fx.world->unpark_chunk(edited->get_id()));
    ASSERT_TRUE(fx.session->CaptureFarWorldPublication(request, unparked, diagnostic))
        << diagnostic;
    EXPECT_EQ(unparked->tiles[0].observation->content_identity(),
              parked->tiles[0].observation->content_identity());
    EXPECT_EQ(fx.world->find_authoritative_chunk(edited->get_coords()), edited);
}

TEST(SavedFarWorldPublication, TallDeepAndReturningKeysKeepAffectedSpansMonotone) {
    DurableFixture fx(false);
    ASSERT_TRUE(fx.created) << fx.error;
    fx.session->EnableFarWorldPersistenceSafety();
    const std::array keys{FarTileKey{5, 0, 0}, FarTileKey{5, 1, 0}};
    auto request = SavedPublicationRequest(keys);
    std::shared_ptr<const FarWorldPublication> before, after;
    std::string diagnostic;
    ASSERT_TRUE(fx.session->CaptureFarWorldPublication(request, before, diagnostic)) << diagnostic;
    ASSERT_TRUE(AdoptPublicationEdit(fx, {4, 40, 4}));
    ASSERT_TRUE(AdoptPublicationEdit(fx, {4, -40, 4}));
    ASSERT_TRUE(fx.session->CaptureFarWorldPublication(request, after, diagnostic)) << diagnostic;
    ASSERT_EQ(before->tiles.size(), 2u);
    ASSERT_EQ(after->tiles.size(), 2u);
    const auto affected = after->tiles[0].span.Intent();
    ASSERT_TRUE(affected);
    ASSERT_TRUE(affected->extra_span);
    EXPECT_LE(affected->extra_span->min_y, -640);
    EXPECT_GE(affected->extra_span->max_y, 656);
    EXPECT_EQ(before->tiles[1].span.Intent(), after->tiles[1].span.Intent());
    const auto unchanged = after->tiles[1].observation->content_identity();
    EXPECT_EQ(before->tiles[1].observation->content_identity(), unchanged);
    // Leaving wanted must not reset this key's history when it returns.
    request.wanted = std::span<const FarTileKey>(keys).subspan(1);
    ASSERT_TRUE(fx.session->CaptureFarWorldPublication(request, after, diagnostic)) << diagnostic;
    request.wanted = keys;
    ASSERT_TRUE(fx.session->CaptureFarWorldPublication(request, after, diagnostic)) << diagnostic;
    EXPECT_EQ(after->tiles[0].span.Intent(), affected);
}

TEST(SavedFarWorldPublication, UnavailableEditedAuthorityNeverSubstitutesPristine) {
    DurableFixture fx(false);
    ASSERT_TRUE(fx.created) << fx.error;
    fx.session->EnableFarWorldPersistenceSafety();
    const std::array keys{FarTileKey{5, 0, 0}};
    auto request = SavedPublicationRequest(keys);
    std::shared_ptr<const FarWorldPublication> publication;
    std::string diagnostic;
    ASSERT_TRUE(fx.session->CaptureFarWorldPublication(request, publication, diagnostic))
        << diagnostic;
    const auto retained = publication;
    const auto bad = AdoptPublicationEdit(fx, {4, 0, 4});
    ASSERT_TRUE(bad);
    bad->sdf_data.pop_back();
    EXPECT_FALSE(fx.session->CaptureFarWorldPublication(request, publication, diagnostic));
    EXPECT_EQ(publication, retained);
    EXPECT_FALSE(diagnostic.empty());
    EXPECT_TRUE(bad->is_voxel_data_dirty());
    EXPECT_EQ(fx.world->find_authoritative_chunk(bad->get_coords()), bad);
}

TEST(SavedFarWorldPublication, OutOfBudgetColumnEditRefusesAndReleasesReplacementCharges) {
    DurableFixture fx(false);
    ASSERT_TRUE(fx.created) << fx.error;
    fx.session->EnableFarWorldPersistenceSafety();
    const std::array keys{FarTileKey{5, 0, 0}};
    auto request = SavedPublicationRequest(keys);
    std::shared_ptr<const FarWorldPublication> publication;
    std::string diagnostic;
    ASSERT_TRUE(fx.session->CaptureFarWorldPublication(request, publication, diagnostic))
        << diagnostic;
    const auto retained = publication;
    ASSERT_TRUE(AdoptPublicationEdit(fx, {4, 1000, 4}));
    std::array<std::uint64_t, 5> charges{};
    const auto source_charge = request.source_memory->used();
    for (std::size_t i = 0; i < charges.size(); ++i)
        charges[i] = request.memory[i]->used();
    EXPECT_FALSE(fx.session->CaptureFarWorldPublication(request, publication, diagnostic));
    EXPECT_EQ(publication, retained);
    EXPECT_FALSE(diagnostic.empty());
    EXPECT_EQ(request.source_memory->used(), source_charge);
    for (std::size_t i = 0; i < charges.size(); ++i)
        EXPECT_EQ(request.memory[i]->used(), charges[i]);
}

TEST(SavedFarWorldPublication, CaptureDoesNotChangeSimulationHashesOrScheduler) {
    DurableFixture fx(false);
    ASSERT_TRUE(fx.created) << fx.error;
    fx.session->EnableFarWorldPersistenceSafety();
    ASSERT_TRUE(AdoptPublicationEdit(fx, {4, 0, 4}));
    const auto hash = fx.session->ComputeChunkStateHash();
    const auto water = fx.world->debug_water_state_hash();
    const auto schedule = fx.session->GetRegionSchedule();
    const auto tick = fx.session->GetSimulationTickCount();
    const std::array keys{FarTileKey{5, 0, 0}};
    auto request = SavedPublicationRequest(keys);
    std::shared_ptr<const FarWorldPublication> publication;
    std::string diagnostic;
    ASSERT_TRUE(fx.session->CaptureFarWorldPublication(request, publication, diagnostic))
        << diagnostic;
    EXPECT_EQ(fx.session->ComputeChunkStateHash(), hash);
    EXPECT_EQ(fx.world->debug_water_state_hash().hash, water.hash);
    EXPECT_EQ(fx.session->GetRegionSchedule(), schedule);
    EXPECT_EQ(fx.session->GetSimulationTickCount(), tick);
    EXPECT_FALSE(fx.session->ActiveRegionsEnabled());
}

TEST(SavedFarWorldPublication, WaterOnlyPreviewChangesCurrencyWithoutDurableGeneration) {
    DurableFixture fx(false);
    ASSERT_TRUE(fx.created) << fx.error;
    fx.session->EnableFarWorldPersistenceSafety();
    const auto chunk = AdoptPublicationEdit(fx, {4, 0, 4});
    ASSERT_TRUE(chunk);
    chunk->has_water_sim.store(true);
    chunk->current_water_resolution.store(8);
    chunk->water_depth_mm.assign(64, 1000);
    chunk->water_bed_mm.assign(64, 100000);
    chunk->water_edge_flux.assign(128, 0);
    chunk->water_src_mm.assign(64, 0);
    chunk->water_level_data.assign(64, 101);
    chunk->water_rest_level.assign(64, 101);
    chunk->water_sim_terrain_height.assign(64, 100);
    chunk->water_flow_data.assign(64, Luminumbra::Vec2(0));
    chunk->mark_water_state_dirty();
    const std::array keys{FarTileKey{5, 0, 0}};
    auto request = SavedPublicationRequest(keys);
    std::shared_ptr<const FarWorldPublication> wet, legacy, dry;
    std::string diagnostic;
    ASSERT_TRUE(fx.session->CaptureFarWorldPublication(request, wet, diagnostic)) << diagnostic;
    ASSERT_EQ(wet->tiles.size(), 1u);
    const auto water = wet->tiles[0].observation->WaterSample(0, 64, 64);
    ASSERT_TRUE(water);
    EXPECT_TRUE(water->wet);
    EXPECT_FLOAT_EQ(water->surface_height, 101);
    const auto terrain = chunk->sdf_data;
    // Older saves retain authoritative float mirrors without integer grids.
    chunk->water_depth_mm.clear();
    chunk->water_bed_mm.clear();
    chunk->water_edge_flux.clear();
    ASSERT_TRUE(fx.session->CaptureFarWorldPublication(request, legacy, diagnostic)) << diagnostic;
    ASSERT_EQ(legacy->tiles.size(), 1u);
    EXPECT_EQ(wet->tiles[0].observation->content_identity(),
              legacy->tiles[0].observation->content_identity());
    EXPECT_TRUE(chunk->water_depth_mm.empty());
    chunk->water_bed_mm.assign(64, 100000);
    chunk->water_edge_flux.assign(128, 0);
    chunk->water_depth_mm.assign(64, 0);
    chunk->water_level_data.assign(64, 100);
    chunk->mark_water_state_dirty();
    ASSERT_TRUE(fx.session->CaptureFarWorldPublication(request, dry, diagnostic)) << diagnostic;
    ASSERT_EQ(dry->tiles.size(), 1u);
    const auto dry_water = dry->tiles[0].observation->WaterSample(0, 64, 64);
    ASSERT_TRUE(dry_water);
    EXPECT_FALSE(dry_water->wet);
    EXPECT_NE(wet->tiles[0].observation->content_identity(),
              dry->tiles[0].observation->content_identity());
    EXPECT_EQ(wet->committed_generation, dry->committed_generation);
    EXPECT_EQ(chunk->sdf_data, terrain);
    EXPECT_TRUE(chunk->is_water_state_dirty());
}

TEST(SavedFarWorldPublication, FlagOffSafetyPreservesLegacySaveBytesAndLoadsWithoutCatalog) {
    DurableFixture legacy(false), safe(false);
    ASSERT_TRUE(legacy.created) << legacy.error;
    ASSERT_TRUE(safe.created) << safe.error;
    safe.session->EnableFarWorldPersistenceSafety();
    ASSERT_TRUE(AdoptPublicationEdit(legacy, {4, 0, 4}));
    ASSERT_TRUE(AdoptPublicationEdit(safe, {4, 0, 4}));
    EXPECT_EQ(legacy.session->ComputeChunkStateHash(), safe.session->ComputeChunkStateHash());
    ASSERT_TRUE(legacy.session->SaveWorldState());
    ASSERT_TRUE(safe.session->SaveWorldState());
    using Service = Luminumbra::Persistence::WorldSaveService;
    const auto legacy_root = legacy.session->GetWorldSaveDir();
    const auto safe_root = safe.session->GetWorldSaveDir();
    const auto bytes =
        Luminumbra::TestSupport::FarCacheRead(Service::region_file_path(legacy_root, 0, 0));
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(bytes,
              Luminumbra::TestSupport::FarCacheRead(Service::region_file_path(safe_root, 0, 0)));
    EXPECT_FALSE(Luminumbra::Persistence::snapshot_index_exists(safe_root));
    EXPECT_FALSE(fs::exists(safe_root / "far"));
    ASSERT_TRUE(safe.session->LoadWorld(safe.session->GetMetadata().worldId));
    const auto loaded = safe.session->GetWorldSystem()->find_authoritative_chunk({4, 0, 4});
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->sdf_data.front(), -3);
}

TEST(SavedFarWorldPublication, SafetyLimitAndSaveFailureKeepDirtyObligationsWithoutActiveRegions) {
    DurableFixture fx(false);
    ASSERT_TRUE(fx.created) << fx.error;
    fx.session->EnableFarWorldPersistenceSafety();
    fx.session->SetParkedBytesLimit(1);
    fx.Settle(fx.SpawnAnchor(), kSettleIterations);
    const auto edited = fx.EditAtSpawnAndFindEdited();
    ASSERT_TRUE(edited);
    fx.Settle(fx.FarAnchor(), kSettleIterations);
    EXPECT_TRUE(fx.world->find_streamed_chunk(edited->get_coords()));
    EXPECT_TRUE(fx.session->PersistenceSaveRequested());
    const auto blocked = fx.session->GetWorldSaveDir() / "blocked_destination";
    {
        std::ofstream file(blocked);
        file << "occupied";
    }
    EXPECT_FALSE(fx.session->SaveWorldStateTo(blocked));
    EXPECT_TRUE(edited->is_voxel_data_dirty());
    EXPECT_EQ(fx.world->find_authoritative_chunk(edited->get_coords()), edited);
    EXPECT_FALSE(fx.session->ActiveRegionsEnabled());
}

TEST(SavedFarWorldPublication, StaleTerrainAndWaterRevisionsCannotClaimLiveOwnership) {
    DurableFixture fx(false);
    ASSERT_TRUE(fx.created) << fx.error;
    fx.session->EnableFarWorldPersistenceSafety();
    fx.Settle(fx.SpawnAnchor(), kSettleIterations);
    std::shared_ptr<Chunk> current;
    for (const auto& chunk : fx.world->snapshot_streamed_chunks())
        if (chunk && fx.world->far_live_mesh_current(*chunk, false) &&
            fx.world->far_live_mesh_current(*chunk, true)) {
            current = chunk;
            break;
        }
    ASSERT_TRUE(current);
    current->mark_water_state_dirty();
    EXPECT_TRUE(fx.world->far_live_mesh_current(*current, false));
    EXPECT_FALSE(fx.world->far_live_mesh_current(*current, true));
    current->mark_voxel_data_dirty();
    EXPECT_FALSE(fx.world->far_live_mesh_current(*current, false));
}

TEST(SavedFarWorldPublication, LegacySafetyLoadAcceptsScentDepositsAndMatchesLegacyHashes) {
    DurableFixture fixture(false);
    ASSERT_TRUE(fixture.created) << fixture.error;
    ASSERT_TRUE(fixture.session->SaveWorldState());
    const auto root = fixture.session->GetWorldSaveDir();
    EXPECT_FALSE(Luminumbra::Persistence::snapshot_index_exists(root));
    EXPECT_FALSE(fs::exists(root / "far"));
    GameSession safe;
    safe.SetJobSystem(&fixture.jobs);
    safe.SetRootPath(fixture.root.root_string());
    safe.EnableFarWorldPersistenceSafety();
    const auto id = fixture.session->GetMetadata().worldId;
    ASSERT_TRUE(fixture.session->LoadWorld(id)); // Original flag-off load path.
    ASSERT_TRUE(safe.LoadWorld(id));             // Candidate publication with safety enabled.
    EXPECT_FALSE(safe.ActiveRegionsEnabled());
    for (auto* session : {fixture.session.get(), &safe}) {
        const auto entity = session->GetRegistry().create();
        session->GetRegistry()
            .emplace<Luminumbra::Components::TransformComponent>(entity)
            .position = session->GetMetadata().spawnPoint;
        session->GetRegistry()
            .emplace<Luminumbra::Components::ScentSenseComponent>(entity)
            .channel = 0;
        auto* scent = session->GetScentField();
        ASSERT_TRUE(scent);
        EXPECT_TRUE(scent->SimulationCellAllowed(64, 64));
        scent->Deposit(0, 64, 64, 7.5);
        scent->Deposit(1, 63, 64, 2.25);
        EXPECT_DOUBLE_EQ(scent->Sample(0, 64, 64), 7.5);
        EXPECT_DOUBLE_EQ(scent->Sample(1, 63, 64), 2.25);
    }
    for (int tick = 0; tick < 4; ++tick) {
        EXPECT_FALSE(safe.ComputeScentSubHash().empty());
        EXPECT_EQ(safe.ComputeScentSubHash(), fixture.session->ComputeScentSubHash());
        EXPECT_EQ(safe.ComputeChunkStateHash(), fixture.session->ComputeChunkStateHash());
        EXPECT_EQ(safe.GetWorldSystem()->debug_water_state_hash().hash,
                  fixture.session->GetWorldSystem()->debug_water_state_hash().hash);
        EXPECT_EQ(safe.GetRegionSchedule(), fixture.session->GetRegionSchedule());
        for (auto* session : {fixture.session.get(), &safe})
            ASSERT_EQ(session->TickSimulation(session->GetSimulationClock().fixed_dt()), 1u);
    }
    for (int ch = 0; ch < safe.GetScentField()->channels(); ++ch)
        for (int z = 0; z < safe.GetScentField()->height(); ++z)
            for (int x = 0; x < safe.GetScentField()->width(); ++x)
                EXPECT_DOUBLE_EQ(safe.GetScentField()->Sample(ch, x, z),
                                 fixture.session->GetScentField()->Sample(ch, x, z));
}

TEST(SavedFarWorldPublication,
     CommittedRecordAdmissionCountsSelectedAuthorityWithoutPristineDecode) {
    DurableFixture fixture(false);
    ASSERT_TRUE(fixture.created) << fixture.error;
    fixture.session->EnableFarWorldPersistenceSafety();
    const auto selected = AdoptPublicationEdit(fixture, {4, 0, 4});
    const auto other = AdoptPublicationEdit(fixture, {20, 0, 4});
    ASSERT_TRUE(selected);
    ASSERT_TRUE(other);
    auto pristine = std::make_shared<Chunk>(IVec3(6, 0, 4));
    fixture.world->GenerateChunkData(*pristine, 4);
    pristine->set_state(ChunkState::Idle);
    ASSERT_TRUE(fixture.world->adopt_streamed_chunk(pristine));
    ASSERT_TRUE(fixture.session->SaveWorldState());
    using Service = Luminumbra::Persistence::WorldSaveService;
    const auto path = Service::region_file_path(fixture.session->GetWorldSaveDir(), 0, 0);
    const auto limits = GameSession::FarWorldPublicationLimits().source;
    Service::EditedRegionReadLimits read_limits{limits.max_file_bytes,
                                                limits.max_decode_bytes,
                                                limits.max_scratch_bytes,
                                                1,
                                                limits.max_samples};
    std::size_t records = 0, visits = 0, classifications = 0;
    std::uint64_t decoded = 0;
    Luminumbra::Persistence::MemberDigest digest;
    const auto read = [&] {
        return Service::read_edited_source_region(
            path,
            read_limits,
            false,
            [&](const Chunk&, std::uint64_t) {
                ++visits;
                return true;
            },
            digest,
            decoded,
            records);
    };
    EXPECT_FALSE(read()); // Default callers still select every edited header.
    EXPECT_EQ(visits, 0u);
    read_limits.selected = [&](const IVec3& coords) {
        ++classifications;
        return coords == selected->get_coords();
    };
    ASSERT_TRUE(read());
    EXPECT_EQ(records, 1u);
    EXPECT_EQ(classifications, 2u); // Pristine headers never even enter selection.
    const std::array keys{FarTileKey{1, 0, 0}};
    auto request = SavedPublicationRequest(keys);
    request.limits.source.max_records = 1;
    std::shared_ptr<const FarWorldPublication> publication;
    std::string diagnostic;
    ASSERT_TRUE(fixture.session->CaptureFarWorldPublication(request, publication, diagnostic))
        << diagnostic;
    ASSERT_TRUE(publication);
    EXPECT_EQ(visits, 2u); // Unselected edited payloads still undergo validation.
    std::vector<Service::ContainerRecord> raw;
    ASSERT_TRUE(Service::read_container_records(path, raw));
    std::uint64_t edited_bytes = 0;
    for (auto& record : raw) {
        if (record.id == pristine->get_id())
            record.payload = "invalid cached JSON";
        else
            edited_bytes += record.payload.size();
    }
    EXPECT_EQ(decoded, edited_bytes);
    ASSERT_TRUE(Service::upsert_container_records(path, raw));
    EXPECT_TRUE(read()); // The manifest already proves the damaged cache is pristine.
    for (auto& record : raw)
        if (record.id == other->get_id())
            record.payload = "invalid authoritative JSON";
    ASSERT_TRUE(Service::upsert_container_records(path, raw));
    EXPECT_FALSE(read()); // Unavailable edited data stays an error, even outside selection.
}

TEST(SavedFarWorldPublication, IncrementalTokenPollVisitsNoChunksAndTracksEveryMutation) {
    DurableFixture fixture(false);
    ASSERT_TRUE(fixture.created) << fixture.error;
    fixture.session->EnableFarWorldPersistenceSafety();
    for (int x = 0; x < 1875; ++x) {
        auto chunk = std::make_shared<Chunk>(IVec3(x, 0, 40));
        chunk->set_state(ChunkState::Idle);
        ASSERT_TRUE(fixture.world->adopt_streamed_chunk(chunk));
    }
    const auto poll = [&] {
        const auto work = Chunk::debug_authority_read_count();
        const auto token = fixture.world->far_world_publication_token();
        EXPECT_EQ(Chunk::debug_authority_read_count() - work, 0u);
        return token;
    };
    auto previous = poll();
    const auto changed = [&] {
        const auto next = poll();
        EXPECT_NE(next, previous);
        previous = next;
        for (int frame = 0; frame < 120; ++frame)
            EXPECT_EQ(poll(), previous);
    };
    auto chunk = std::make_shared<Chunk>(IVec3(4, 0, 4));
    chunk->mark_sdf_loaded_or_edited();
    chunk->set_state(ChunkState::Idle);
    ASSERT_TRUE(fixture.world->adopt_streamed_chunk(chunk));
    changed();
    chunk->mark_voxel_data_dirty();
    changed();
    chunk->mark_voxel_data_dirty(); // Already dirty: the revision must still notify.
    changed();
    chunk->clear_voxel_data_dirty();
    changed();
    chunk->mark_dirty_for_save();
    changed();
    chunk->clear_voxel_data_dirty_after_generation();
    changed();
    chunk->mark_water_state_dirty();
    changed();
    chunk->mark_water_state_dirty();
    changed();
    chunk->clear_water_state_dirty();
    changed();
    chunk->mark_sdf_generated_current_params();
    changed();
    chunk->mark_sdf_loaded_or_edited();
    changed();
    chunk->set_water_authority();
    changed();
    chunk->mark_sdf_generated_current_params(); // Sticky water authority remains observed.
    changed();
    chunk->set_state(ChunkState::Loading);
    changed();
    EXPECT_GT(previous[4], 0u);
    ASSERT_TRUE(chunk->try_set_state(ChunkState::Loading, ChunkState::Idle));
    changed();
    EXPECT_EQ(previous[4], 0u);
    chunk->set_state(ChunkState::Loading);
    changed();
    chunk->set_state(ChunkState::Idle);
    changed(); // Both setters detect loading completion without a residency scan.
    fixture.world->clear_world(fixture.physics);
    changed();
    chunk->mark_water_state_dirty(); // Detached retained handles cannot invalidate this world.
    EXPECT_EQ(poll(), previous);
}

TEST(SavedFarWorldPublication, IncrementalTokenTracksParkUnparkAndDurableRelease) {
    DurableFixture fixture(false);
    ASSERT_TRUE(fixture.created) << fixture.error;
    fixture.session->EnableFarWorldPersistenceSafety();
    fixture.Settle(fixture.SpawnAnchor(), kSettleIterations);
    const auto edited = fixture.EditAtSpawnAndFindEdited();
    ASSERT_TRUE(edited);
    fixture.Settle(fixture.SpawnAnchor(), kSettleIterations);
    auto previous = fixture.world->far_world_publication_token();
    fixture.Settle(fixture.FarAnchor(), kSettleIterations);
    ASSERT_TRUE(Contains(fixture.world->snapshot_parked_chunks(), edited.get()));
    const auto parked = fixture.world->far_world_publication_token();
    EXPECT_GT(parked[1], previous[1]);
    edited->mark_water_state_dirty();
    EXPECT_GT(fixture.world->far_world_publication_token()[0], parked[0]);
    previous = fixture.world->far_world_publication_token();
    ASSERT_TRUE(fixture.world->unpark_chunk(edited->get_id()));
    EXPECT_GT(fixture.world->far_world_publication_token()[1], previous[1]);
    fixture.Settle(fixture.FarAnchor(), kSettleIterations);
    ASSERT_TRUE(Contains(fixture.world->snapshot_parked_chunks(), edited.get()));
    previous = fixture.world->far_world_publication_token();
    ASSERT_TRUE(fixture.session->SaveWorldState());
    EXPECT_FALSE(Contains(fixture.world->snapshot_parked_chunks(), edited.get()));
    EXPECT_GT(fixture.world->far_world_publication_token()[1], previous[1]);
    previous = fixture.world->far_world_publication_token();
    edited->mark_voxel_data_dirty();
    EXPECT_EQ(fixture.world->far_world_publication_token(), previous);
    ASSERT_TRUE(fixture.world->restore_authoritative_chunk(edited->get_coords()));
    EXPECT_GT(fixture.world->far_world_publication_token()[1], previous[1]);
    const auto work = Chunk::debug_authority_read_count();
    previous = fixture.world->far_world_publication_token();
    for (int frame = 0; frame < 120; ++frame)
        EXPECT_EQ(fixture.world->far_world_publication_token(), previous);
    EXPECT_EQ(Chunk::debug_authority_read_count(), work);
}

TEST(SavedFarWorldPublication, UnchangedFramesDoNoCaptureAndChangesCaptureOnce) {
    DurableFixture fixture(false);
    ASSERT_TRUE(fixture.created) << fixture.error;
    fixture.session->EnableFarWorldPersistenceSafety();
    const std::array keys{FarTileKey{5, 0, 0}};
    auto request = SavedPublicationRequest(keys);
    request.incremental = true;
    std::shared_ptr<const FarWorldPublication> publication;
    std::string diagnostic;
    const auto finish = [&] {
        for (std::size_t frame = 0; frame <= request.wanted.size(); ++frame) {
            if (fixture.session->CaptureFarWorldPublication(request, publication, diagnostic))
                return true;
            if (!diagnostic.empty())
                return false;
        }
        return false;
    };
    ASSERT_TRUE(finish()) << diagnostic;
    const auto unchanged = [&] {
        const auto captures = fixture.session->FarWorldCaptureCount();
        const auto observations = fixture.session->FarWorldObservationCaptureCount();
        const auto attempts = request.source_memory->attempts();
        const auto retained = publication;
        const auto chunk_work = Chunk::debug_authority_read_count();
        for (int frame = 0; frame < 120; ++frame) {
            EXPECT_FALSE(
                fixture.session->CaptureFarWorldPublication(request, publication, diagnostic));
            EXPECT_TRUE(diagnostic.empty());
            EXPECT_EQ(publication, retained);
        }
        EXPECT_EQ(Chunk::debug_authority_read_count(), chunk_work);
        EXPECT_EQ(fixture.session->FarWorldCaptureCount(), captures);
        EXPECT_EQ(fixture.session->FarWorldObservationCaptureCount(), observations);
        EXPECT_EQ(request.source_memory->attempts(), attempts);
    };
    unchanged();
    const auto before = fixture.session->FarWorldCaptureCount();
    const auto edited = AdoptPublicationEdit(fixture, {4, 0, 4});
    ASSERT_TRUE(edited);
    ASSERT_TRUE(finish()) << diagnostic;
    EXPECT_EQ(fixture.session->FarWorldCaptureCount(), before + 1);
    unchanged();
    // A direct water writer changes the metadata token without a regional scheduler.
    const auto before_water = fixture.session->FarWorldCaptureCount();
    edited->mark_water_state_dirty();
    ASSERT_TRUE(finish()) << diagnostic;
    EXPECT_EQ(fixture.session->FarWorldCaptureCount(), before_water + 1);
    unchanged();
    // Pristine generation publication is a transient refusal, even though it changes
    // neither edited revisions nor the camera/wanted inputs.
    auto loading = std::make_shared<Chunk>(IVec3(6, 0, 4));
    fixture.world->GenerateChunkData(*loading, 4);
    loading->set_state(ChunkState::Idle);
    ASSERT_TRUE(fixture.world->adopt_streamed_chunk(loading));
    loading->set_state(ChunkState::Loading);
    ASSERT_FALSE(loading->needs_authority());
    const auto loading_token = fixture.world->far_world_publication_token();
    const auto before_loading = fixture.session->FarWorldCaptureCount();
    EXPECT_FALSE(finish());
    EXPECT_NE(diagnostic.find("generation publication is pending"), std::string::npos);
    EXPECT_EQ(fixture.session->FarWorldCaptureCount(), before_loading + 1);
    unchanged(); // No repeated quiescing/capture while the same generation remains pending.
    loading->set_state(ChunkState::Idle);
    const auto completed_token = fixture.world->far_world_publication_token();
    EXPECT_EQ(loading_token[0], completed_token[0]);
    EXPECT_GT(completed_token[1], loading_token[1]);
    EXPECT_EQ(loading_token[2], completed_token[2]);
    EXPECT_EQ(loading_token[3], completed_token[3]);
    EXPECT_EQ(completed_token[4], 0u);
    ASSERT_TRUE(finish()) << diagnostic;
    EXPECT_EQ(fixture.session->FarWorldCaptureCount(), before_loading + 2);
    unchanged();
    // Refusal is also remembered. Repair is an explicit source change and retries once.
    edited->sdf_data.pop_back();
    edited->mark_voxel_data_dirty();
    const auto before_refusal = fixture.session->FarWorldCaptureCount();
    EXPECT_FALSE(finish());
    EXPECT_FALSE(diagnostic.empty());
    EXPECT_EQ(fixture.session->FarWorldCaptureCount(), before_refusal + 1);
    unchanged();
    edited->sdf_data.push_back(-3.0f);
    edited->mark_voxel_data_dirty();
    ASSERT_TRUE(finish()) << diagnostic;
    EXPECT_EQ(fixture.session->FarWorldCaptureCount(), before_refusal + 2);
    unchanged();
    // A real admission refusal also suppresses every unchanged frame's retry.
    const auto before_admission = fixture.session->FarWorldCaptureCount();
    request.source_memory->RefuseAttemptsFrom(request.source_memory->attempts());
    edited->mark_voxel_data_dirty();
    EXPECT_FALSE(finish());
    EXPECT_FALSE(diagnostic.empty());
    EXPECT_EQ(fixture.session->FarWorldCaptureCount(), before_admission + 1);
    unchanged();
    request.source_memory->RefuseAttemptsFrom(std::numeric_limits<std::uint64_t>::max());
    unchanged(); // Available memory alone does not cause frame-by-frame polling.
    edited->mark_voxel_data_dirty();
    ASSERT_TRUE(finish()) << diagnostic;
    EXPECT_EQ(fixture.session->FarWorldCaptureCount(), before_admission + 2);
    unchanged();
    // A wanted-set change shares the old key's observation without resampling it.
    const auto old_observation = publication->tiles[0].observation;
    const auto observations = fixture.session->FarWorldObservationCaptureCount();
    const std::array expanded{FarTileKey{5, 0, 0}, FarTileKey{5, 1, 0}};
    request.wanted = expanded;
    ASSERT_TRUE(finish()) << diagnostic;
    ASSERT_EQ(publication->tiles.size(), expanded.size());
    EXPECT_EQ(publication->tiles[0].observation, old_observation);
    EXPECT_EQ(fixture.session->FarWorldObservationCaptureCount(), observations + 1);
    unchanged();
}

TEST(SavedFarWorldPublication, SimultaneousAdjacentWaterChangesRetryCaptureOnce) {
    DurableFixture fixture(false);
    ASSERT_TRUE(fixture.created) << fixture.error;
    fixture.session->EnableFarWorldPersistenceSafety();
    const auto left = AdoptPublicationEdit(fixture, {4, 0, 4});
    const auto right = AdoptPublicationEdit(fixture, {5, 0, 4});
    ASSERT_TRUE(left);
    ASSERT_TRUE(right);
    ASSERT_EQ(left->voxel_revision(), right->voxel_revision());
    for (const auto& chunk : {left, right}) {
        chunk->has_water_sim.store(true);
        chunk->current_water_resolution.store(8);
        chunk->water_depth_mm.assign(64, 1000);
        chunk->water_bed_mm.assign(64, 100000);
        chunk->water_edge_flux.assign(128, 0);
        chunk->water_src_mm.assign(64, 0);
        chunk->water_level_data.assign(64, 101);
        chunk->water_rest_level.assign(64, 101);
        chunk->water_sim_terrain_height.assign(64, 100);
        chunk->water_flow_data.assign(64, Luminumbra::Vec2(0));
        chunk->mark_water_state_dirty();
        chunk->mark_water_state_dirty();
        ASSERT_EQ(chunk->water_revision(), 2u);
        ASSERT_TRUE(chunk->is_voxel_data_dirty());
        ASSERT_TRUE(chunk->is_water_state_dirty());
    }
    const std::array keys{FarTileKey{5, 0, 0}};
    auto request = SavedPublicationRequest(keys);
    request.incremental = true;
    std::shared_ptr<const FarWorldPublication> publication;
    std::string diagnostic;
    const auto finish = [&] {
        for (std::size_t frame = 0; frame <= request.wanted.size(); ++frame) {
            if (fixture.session->CaptureFarWorldPublication(request, publication, diagnostic))
                return true;
            if (!diagnostic.empty())
                return false;
        }
        return false;
    };
    ASSERT_TRUE(finish()) << diagnostic;
    const auto previous = publication;
    const auto token = fixture.world->far_world_publication_token();
    const auto captures = fixture.session->FarWorldCaptureCount();
    for (const auto& chunk : {left, right}) {
        chunk->water_depth_mm.assign(64, 2000);
        chunk->water_level_data.assign(64, 102);
        chunk->mark_water_state_dirty();
        ASSERT_EQ(chunk->water_revision(), 3u);
    }
    const auto changed = fixture.world->far_world_publication_token();
    EXPECT_NE(changed[0], token[0]);
    // Neither dirty membership nor durable/worldgen/loading state changed.
    EXPECT_TRUE(std::equal(token.begin() + 1, token.end(), changed.begin() + 1));
    ASSERT_TRUE(finish()) << diagnostic;
    EXPECT_EQ(fixture.session->FarWorldCaptureCount(), captures + 1);
    EXPECT_GT(publication->publication_serial, previous->publication_serial);
    EXPECT_EQ(publication->committed_generation, previous->committed_generation);
    EXPECT_NE(publication->tiles[0].observation->content_identity(),
              previous->tiles[0].observation->content_identity());
    for (int frame = 0; frame < 120; ++frame) {
        EXPECT_FALSE(fixture.session->CaptureFarWorldPublication(request, publication, diagnostic));
        EXPECT_TRUE(diagnostic.empty());
    }
    EXPECT_EQ(fixture.session->FarWorldCaptureCount(), captures + 1);
}

TEST(SavedFarWorldPublication,
     PreviewAdmissionCountsOnlyRelevantAuthorityAndKeepsUnavailableEdits) {
    DurableFixture fixture(false);
    ASSERT_TRUE(fixture.created) << fixture.error;
    fixture.session->EnableFarWorldPersistenceSafety();
    const std::array keys{FarTileKey{5, 0, 0}};
    auto request = SavedPublicationRequest(keys);
    request.limits.source.max_records = 1;
    const auto selected = AdoptPublicationEdit(fixture, {4, 0, 4});
    ASSERT_TRUE(selected);
    // More authority than the record allowance, but outside every wanted support/halo.
    // Malformed unrelated payloads must not consume selected-authority admission.
    for (const IVec3 coords : {IVec3(20000, 0, 0), IVec3(20001, 0, 0)}) {
        auto outside = AdoptPublicationEdit(fixture, coords);
        ASSERT_TRUE(outside);
        outside->sdf_data.clear();
    }
    std::shared_ptr<const FarWorldPublication> publication;
    std::string diagnostic;
    ASSERT_TRUE(fixture.session->CaptureFarWorldPublication(request, publication, diagnostic))
        << diagnostic;
    ASSERT_TRUE(publication);
    const auto retained = publication;
    selected->sdf_data.pop_back();
    selected->mark_voxel_data_dirty();
    EXPECT_FALSE(fixture.session->CaptureFarWorldPublication(request, publication, diagnostic));
    EXPECT_FALSE(diagnostic.empty());
    EXPECT_EQ(publication, retained);
    EXPECT_TRUE(selected->is_voxel_data_dirty());
    // A second relevant edit exceeds the unchanged cap; never silently omit it.
    selected->sdf_data.push_back(-3.0f);
    selected->mark_voxel_data_dirty();
    ASSERT_TRUE(AdoptPublicationEdit(fixture, {6, 0, 4}));
    EXPECT_FALSE(fixture.session->CaptureFarWorldPublication(request, publication, diagnostic));
    EXPECT_FALSE(diagnostic.empty());
    EXPECT_EQ(publication, retained);
}

TEST(SavedFarWorldPublication,
     CompactRenderSamplesMatchExistingNumericConversionAndRetainRawCurrency) {
    using namespace Luminumbra::World;
    for (const bool striped_material : {false, true}) {
        FarFrozenCaptureSource source;
        source.use = FarFrozenUse::Preview;
        source.pristine.numeric = FarVolumeNumericProfile::LegacySigned16;
        source.pristine.height = [](std::int64_t, std::int64_t) {
            return 8.0f;
        };
        source.pristine.density =
            [striped_material](const FarVolumePosition& p, float, std::uint32_t, FarCaveMode) {
                const auto material = striped_material
                                          ? static_cast<std::uint8_t>((p.x / 4 + 64) % 256)
                                          : std::uint8_t{7};
                return FarVolumeSample{static_cast<float>(p.y) - 0.125f, material};
            };
        source.water = [](std::int64_t, std::int64_t) {
            return FarFrozenWaterSample{false, 0};
        };
        const FarFrozenRegion region{1, {{0, -256, 0}, {64, 16, 16}}};
        const auto limits = GameSession::FarWorldPublicationLimits().frozen;
        auto pool = std::make_shared<FarVolumeMemoryPool>(256ull * 1024 * 1024);
        std::shared_ptr<const FarFrozenObservation> original, compact, changed;
        ASSERT_EQ(CaptureFarFrozenObservation({&region, 1}, source, limits, pool, original),
                  FarFrozenCaptureError::None);
        source.render_only = true;
        ASSERT_EQ(CaptureFarFrozenObservation({&region, 1}, source, limits, pool, compact),
                  FarFrozenCaptureError::None);
        EXPECT_LT(compact->payload_bytes(), original->payload_bytes());
        for (std::int64_t z = -4; z <= 20; z += 4)
            for (std::int64_t y = -260; y <= 20; y += 4)
                for (std::int64_t x = -4; x <= 68; x += 4) {
                    const FarVolumePosition p{x, y, z};
                    const auto raw = original->Sample(1, p), sample = compact->Sample(1, p);
                    ASSERT_TRUE(raw);
                    ASSERT_TRUE(sample);
                    EXPECT_FLOAT_EQ(sample->density,
                                    DequantizeFarLodSdf(QuantizeFarLodSdf(raw->density)));
                    EXPECT_EQ(sample->material, raw->material);
                    EXPECT_FALSE(compact->Sample(1, p, true));
                }
        EXPECT_EQ(original->WaterSample(0, 0, 0), compact->WaterSample(0, 0, 0));
        source.pristine.density =
            [striped_material](const FarVolumePosition& p, float, std::uint32_t, FarCaveMode) {
                const auto material = striped_material
                                          ? static_cast<std::uint8_t>((p.x / 4 + 64) % 256)
                                          : std::uint8_t{7};
                return FarVolumeSample{static_cast<float>(p.y) - 0.125f + 0.0001f, material};
            };
        ASSERT_EQ(CaptureFarFrozenObservation({&region, 1}, source, limits, pool, changed),
                  FarFrozenCaptureError::None);
        EXPECT_EQ(compact->Sample(1, {0, 0, 0}), changed->Sample(1, {0, 0, 0}));
        EXPECT_NE(compact->content_identity(), changed->content_identity());
    }
}
