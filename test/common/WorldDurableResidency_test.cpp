// Durable residency (S3): with set_durable_residency(true), an edited (LoadedOrEdited)
// chunk that streams out of the radius is PARKED as normalized sim truth instead of
// being erased, and returns byte-identically when the anchor comes back. Pristine
// (regenerable) chunks are never parked. A parked-bytes budget that is too small keeps
// dirty chunks resident and requests a persistence save. Also pins the chunk-level
// contracts the parking path relies on (normalize_for_park, the water revision and the
// water-authority promotion).
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../support/UniqueTempPath.h"
#include "../support/WorldgenTestData.h"
#include "luminumbra/core/Types.h"
#include "luminumbra_client/app/SessionExit.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
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

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

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

// Temp root containing the world preset and its simulation dependencies.
// The root is unique per fixture instance so concurrent common_tests processes
// never share (or clobber) a directory. The destructor removes the tree.
class HeadlessRoot {
public:
    HeadlessRoot() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = fs::temp_directory_path() /
                ("luminumbra_durable_residency_test_" + std::to_string(stamp));
        fs::create_directories(root_ / "worlds" / "atlas" / "presets");
        fs::copy_file(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds" / "atlas" / "presets" /
                          "default.json",
                      root_ / "worlds" / "atlas" / "presets" / "default.json");
        Luminumbra::TestSupport::CopyWorldgenTestData(LUMINUMBRA_SOURCE_ROOT, root_);
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

// Streaming cap in chunks (debug_set_streaming_radius_cap). Small enough that the
// far anchor evicts everything the spawn settle made resident.
constexpr int kStreamRadiusCap = 8;
// Far anchor distance in chunks along X: well outside radius + 2 of the spawn anchor.
constexpr float kFarChunks = 40.0f;
constexpr int kSettleIterations = 40;
constexpr int kReturnIterations = 60;

// One headless world per TEST. Member order: root outlives the session, the job
// system outlives the session, and the destructor tears the session down before
// shutting the job system down.
class DurableFixture {
public:
    explicit DurableFixture(bool durable, bool active_regions = false) {
        jobs.startup();
        session = std::make_unique<GameSession>();
        session->SetActiveRegionsEnabled(active_regions);
        session->SetJobSystem(&jobs);
        session->SetRootPath(root.root_string());
        created = session->CreateWorld("DurableResidency", "12345", "default");
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

    // Select an actual voxel edit among the settled residents. Loaded or water-authoritative
    // chunks can be clean; choosing by provenance would not identify the edit under test.
    std::shared_ptr<Chunk> EditAtSpawnAndFindEdited(int* modified_count = nullptr) {
        const auto before = world->snapshot_streamed_chunks();
        std::map<const Chunk*, std::uint32_t> revisions;
        for (const auto& chunk : before)
            if (chunk)
                revisions.emplace(chunk.get(), chunk->voxel_revision());
        const int modified = world->EditTerrainVoxel(SpawnSurfacePoint(), 3.0f, false, physics);
        if (modified_count)
            *modified_count = modified;
        if (modified <= 0)
            return nullptr;
        for (const auto& chunk : world->snapshot_streamed_chunks()) {
            if (!chunk)
                continue;
            const auto previous = revisions.find(chunk.get());
            if (previous != revisions.end() && chunk->voxel_revision() > previous->second)
                return chunk;
        }
        return nullptr;
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

    int modified = 0;
    const auto edited = fx.EditAtSpawnAndFindEdited(&modified);
    ASSERT_GT(modified, 0);
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

    fx.world->sync_water_authority();
    const auto water_inputs = fx.world->water_authority_input_items();
    edited->mark_water_state_dirty();
    ASSERT_TRUE(fx.world->unpark_chunk(edited->get_id()));
    fx.world->sync_water_authority();
    EXPECT_EQ(fx.world->water_authority_input_items() - water_inputs, 1u);
    EXPECT_TRUE(edited->has_water_authority());

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
    // Hash reads observe the last completed boundary; settle it as a finished tick would.
    fx.world->sync_water_authority();
    const auto before = fx.session->ComputeChunkStateHash();
    const auto parked_bytes = fx.world->parking_status().parked_bytes;
    const auto generation = fx.world->debug_dirty_generation();
    fx.world->release_clean_parked_chunks();
    EXPECT_EQ(fx.world->debug_dirty_generation(), generation);
    ASSERT_TRUE(fx.session->SaveWorldState());
    EXPECT_EQ(fx.world->debug_dirty_generation(), generation + 1);
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
    fx.Settle(fx.FarAnchor(), kSettleIterations);
    // Water that moved makes a chunk authoritative; everything else is regenerable and erased.
    for (const auto& parked : fx.world->snapshot_parked_chunks()) {
        ASSERT_TRUE(parked);
        EXPECT_TRUE(parked->needs_authority());
    }
}

TEST(WorldDurableResidency, ByteLimitKeepsDirtyChunkResidentAndRequestsSave) {
    DurableFixture fx(/*durable=*/true, /*active_regions=*/true);
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

    // Deferred evictions must retry even when a successful save changes no suspension state
    // and has no parked payload to release.
    const auto generation = fx.world->debug_dirty_generation();
    ASSERT_TRUE(fx.session->SaveWorldState());
    EXPECT_EQ(fx.world->debug_dirty_generation(), generation);
    fx.Settle(fx.FarAnchor(), kSettleIterations);
    EXPECT_FALSE(fx.world->find_streamed_chunk(coords));
    EXPECT_FALSE(Contains(fx.world->snapshot_parked_chunks(), edited.get()));
}

TEST(WorldDurableResidency, ClientRefusalGatesDueAndPressureSaves) {
    EXPECT_FALSE(Luminumbra::Client::SaveSessionWhenDue(nullptr, true));
    for (const bool pressure : {false, true}) {
        SCOPED_TRACE(pressure);
        DurableFixture fx(/*durable=*/true, /*active_regions=*/true);
        ASSERT_TRUE(fx.created) << fx.error;
        fx.Settle(fx.SpawnAnchor(), kSettleIterations);
        ASSERT_TRUE(fx.session->SaveWorld());
        if (!pressure) {
            std::ifstream input(fx.session->GetWorldSaveDir() / "world_info.json");
            auto metadata = nlohmann::json::parse(input);
            input.close();
            Luminumbra::world::WorldClock(9000).write_metadata(metadata);
            auto ledger = fx.session->GetActiveRegionLedger();
            ASSERT_TRUE(Luminumbra::Persistence::WorldSaveService::save_metadata_and_active_regions(
                metadata.dump(), ledger, fx.session->GetWorldSaveDir()));
            ASSERT_TRUE(fx.session->LoadWorldState());
            fx.Settle(fx.SpawnAnchor(), kSettleIterations);
        }
        const auto edited = fx.EditAtSpawnAndFindEdited();
        ASSERT_TRUE(edited);
        if (pressure) {
            fx.session->SetParkedBytesLimit(1);
            fx.Settle(fx.FarAnchor(), kSettleIterations);
            ASSERT_TRUE(fx.session->PersistenceSaveRequested());
        } else {
            ASSERT_TRUE(fx.session->PersistenceAutosaveDue());
        }
        const auto directory = fx.session->GetWorldSaveDir();
        const auto tree = [&] {
            std::map<fs::path, std::string> files;
            for (const auto& entry : fs::recursive_directory_iterator(directory)) {
                if (!entry.is_regular_file())
                    continue;
                std::ifstream input(entry.path(), std::ios::binary);
                std::ostringstream bytes;
                bytes << input.rdbuf();
                files.emplace(entry.path().lexically_relative(directory), bytes.str());
            }
            return files;
        };
        const auto committed = tree();
        const auto revision = edited->voxel_revision();
        // Entry refusal, digest refusal, and resolved-content refusal use this exact client gate.
        for (int refusal = 0; refusal < 3; ++refusal) {
            SCOPED_TRACE(refusal);
            const bool allowed = Luminumbra::Client::SessionPersistenceAllowed(
                true, refusal == 0 ? 2 : 0, refusal == 1, refusal == 2);
            ASSERT_FALSE(allowed);
            EXPECT_FALSE(Luminumbra::Client::SaveSessionWhenDue(fx.session.get(), allowed));
            EXPECT_EQ(tree(), committed);
            EXPECT_TRUE(edited->is_voxel_data_dirty());
            EXPECT_EQ(edited->voxel_revision(), revision);
            if (pressure)
                EXPECT_TRUE(fx.session->PersistenceSaveRequested());
            else
                EXPECT_TRUE(fx.session->PersistenceAutosaveDue());
        }
        EXPECT_TRUE(Luminumbra::Client::SaveSessionWhenDue(fx.session.get(), true));
        EXPECT_NE(tree(), committed);
        EXPECT_FALSE(edited->is_voxel_data_dirty());
        EXPECT_FALSE(fx.session->PersistenceSaveRequested());
        EXPECT_FALSE(fx.session->PersistenceAutosaveDue());
        const auto saved = tree();
        EXPECT_FALSE(Luminumbra::Client::SaveSessionWhenDue(fx.session.get(), true));
        EXPECT_EQ(tree(), saved);
    }
}

TEST(WorldDurableResidency, RegionDigestWorkIsLinearAndPublicationInvalidatesAdmission) {
    namespace P = Luminumbra::Persistence;
    DurableFixture fx(/*durable=*/true, /*active_regions=*/true);
    ASSERT_TRUE(fx.created) << fx.error;
    constexpr int regions = 64;
    for (int region = 0; region < regions; ++region) {
        auto chunk = std::make_shared<Chunk>(IVec3(region * 32, 0, 0));
        chunk->set_state(ChunkState::Idle);
        chunk->heightmap_data = {static_cast<float>(region)};
        chunk->mark_voxel_data_dirty();
        ASSERT_TRUE(fx.world->adopt_streamed_chunk(chunk));
    }
    ASSERT_EQ(fx.world->authority_region_keys().size(), regions);
    const P::RegionDigestInputs complete{fx.world->snapshot_streamed_chunks(),
                                         fx.world->snapshot_parked_chunks(),
                                         fx.world->durable_chunk_inventory()};
    const auto before = fx.world->region_digest_input_items();
    P::RegionDigestOperation operation(fx.session->GetWorldSaveDir(), true);
    for (int region = 0; region < regions; ++region) {
        const Luminumbra::world::RegionKey key{region, 0};
        const auto grouped = fx.world->region_digest_inputs(region, 0);
        ASSERT_EQ(grouped.live.size(), 1u);
        EXPECT_EQ(operation.digest(key, grouped),
                  P::WorldSaveService::region_simulation_digest(
                      fx.session->GetWorldSaveDir(), key, complete));
    }
    EXPECT_EQ(fx.world->region_digest_input_items() - before, regions);
    EXPECT_EQ(operation.input_items(), regions);
    EXPECT_EQ(operation.index_reads(), 1u);
    const auto grouped = fx.world->region_digest_inputs(0, 0);
    const auto expected = operation.digest({0, 0}, grouped);
    ASSERT_TRUE(fx.session->SaveWorldState());
    EXPECT_EQ(operation.digest({0, 0}, grouped), expected);
    EXPECT_EQ(operation.index_reads(), 2u);
    {
        std::ofstream payload(
            P::WorldSaveService::region_file_path(fx.session->GetWorldSaveDir(), 0, 0),
            std::ios::binary | std::ios::trunc);
        payload << "damaged actual member";
        ASSERT_TRUE(payload.good());
    }
    EXPECT_THROW((void)operation.digest({0, 0}, grouped), std::runtime_error);
}

TEST(WorldDurableResidency, GenerationAndTerrainPreparationRegisterLiveDigestAuthority) {
    namespace P = Luminumbra::Persistence;
    for (const int insertion : {0, 1, 2}) {
        SCOPED_TRACE(insertion);
        DurableFixture fx(/*durable=*/true, /*active_regions=*/true);
        ASSERT_TRUE(fx.created) << fx.error;
        const IVec3 coords(128, 0, 128);
        if (insertion == 2) {
            ASSERT_TRUE(fx.world->PrepareTerrainVoxelEdit(Vec3(2056, 8, 2056), 1.0f));
        } else {
            if (insertion == 1) {
                const auto placeholder = std::make_shared<Chunk>(coords);
                ASSERT_TRUE(fx.world->adopt_streamed_chunk(placeholder));
            }
            fx.world->dispatch_generation_jobs(std::vector<IVec3>{coords});
            fx.world->wait_for_streaming_jobs();
        }
        const auto chunk = fx.world->find_streamed_chunk(coords);
        ASSERT_TRUE(chunk);
        ASSERT_FALSE(chunk->sdf_data.empty());
        auto grouped = fx.world->region_digest_inputs(4, 4);
        ASSERT_EQ(grouped.live.size(), 1u);
        EXPECT_EQ(grouped.live.front(), chunk);
        const auto compare_digest = [&] {
            const P::RegionDigestInputs complete{fx.world->snapshot_streamed_chunks(),
                                                 fx.world->snapshot_parked_chunks(),
                                                 fx.world->durable_chunk_inventory()};
            grouped = fx.world->region_digest_inputs(4, 4);
            const auto digest = P::WorldSaveService::region_simulation_digest(
                fx.session->GetWorldSaveDir(), {4, 4}, grouped);
            EXPECT_EQ(digest,
                      P::WorldSaveService::region_simulation_digest(
                          fx.session->GetWorldSaveDir(), {4, 4}, complete));
            return digest;
        };
        chunk->sdf_data.front() = -3.0f;
        chunk->mark_voxel_data_dirty();
        const auto edited = compare_digest();
        ASSERT_TRUE(fx.session->SaveWorldState());
        EXPECT_EQ(compare_digest(), edited);
        chunk->sdf_data.front() = -7.0f;
        chunk->mark_voxel_data_dirty();
        EXPECT_NE(compare_digest(), edited);
        // Water changes on a previously saved generated chunk also override durable bytes.
        const auto terrain_digest = compare_digest();
        chunk->water_depth_mm = {3000};
        chunk->mark_water_state_dirty();
        EXPECT_NE(compare_digest(), terrain_digest);
        fx.world->clear_world(fx.physics);
        EXPECT_TRUE(fx.world->region_digest_inputs(4, 4).live.empty());
        EXPECT_FALSE(fx.world->has_region_members(4, 4));
    }
}

TEST(WorldDurableResidency, RegionDigestExcludesWaterMirrorsAndDirtyBookkeeping) {
    namespace P = Luminumbra::Persistence;
    auto chunk = std::make_shared<Chunk>(IVec3{});
    chunk->set_state(ChunkState::Idle);
    chunk->sdf_data.assign(17u * 17u * 17u, -1.0f);
    chunk->water_depth_mm = {1000};
    chunk->mark_voxel_data_dirty();
    const P::RegionDigestInputs inputs{{chunk}, {}, {}};
    const auto digest = P::WorldSaveService::region_simulation_digest({}, {0, 0}, inputs);
    chunk->water_level_data = {3.0f};
    chunk->mark_water_state_dirty();
    EXPECT_EQ(P::WorldSaveService::region_simulation_digest({}, {0, 0}, inputs), digest);
    chunk->clear_voxel_data_dirty();
    chunk->clear_water_state_dirty();
    EXPECT_EQ(P::WorldSaveService::region_simulation_digest({}, {0, 0}, inputs), digest);
    chunk->water_depth_mm.front() += 1;
    EXPECT_NE(P::WorldSaveService::region_simulation_digest({}, {0, 0}, inputs), digest);
}

TEST(WorldDurableResidency, GeneratedWaterAuthoritySurvivesLodPromotionAndPersistence) {
    namespace P = Luminumbra::Persistence;
    const auto chunk = std::make_shared<Chunk>(IVec3(0, 0, 0));
    chunk->set_state(ChunkState::Ready);
    chunk->heightmap_data.assign(17u * 17u, 8.0f);
    chunk->water_depth_mm = {12};
    chunk->water_bed_mm = {8000};
    chunk->water_edge_flux = {0, 0};
    chunk->mark_sdf_generated_current_params();
    chunk->set_water_authority();
    chunk->clear_voxel_data_dirty();
    Luminumbra::WorldStreamingState state;
    state.insert_chunk(chunk);
    const auto hash = P::ComputeRegionContentHash(state);
    const auto revision = chunk->voxel_revision();
    chunk->sdf_data.assign(17u * 17u * 17u, 3.0f);
    chunk->material_data.assign(17u * 17u * 17u, 1u);
    chunk->mark_sdf_generated_current_params();
    chunk->clear_voxel_data_dirty_after_generation();
    EXPECT_GT(chunk->voxel_revision(), revision);
    EXPECT_TRUE(chunk->is_voxel_data_dirty());
    EXPECT_FALSE(chunk->has_terrain_authority());
    EXPECT_EQ(P::ComputeRegionContentHash(state), hash);
    auto bytes = P::SerializeWorldStreamingStateSnapshotJson(state);
    Luminumbra::WorldStreamingState loaded;
    std::vector<std::string> errors;
    ASSERT_TRUE(P::LoadWorldStreamingStateSnapshotJson(bytes, loaded, errors));
    EXPECT_EQ(P::ComputeRegionContentHash(loaded), hash);
    const auto restored = loaded.snapshot_chunks().front();
    EXPECT_TRUE(restored->has_water_authority());
    EXPECT_FALSE(restored->has_terrain_authority());
    restored->sdf_data.front() = -2.0f;
    restored->mark_voxel_data_dirty();
    EXPECT_NE(P::ComputeRegionContentHash(loaded), hash);
    auto invalid = nlohmann::json::parse(bytes);
    invalid["chunks"][0]["water_state"]["terrain_authoritative"] = "unknown";
    EXPECT_FALSE(P::LoadWorldStreamingStateSnapshotJson(invalid.dump(), loaded, errors));
}

TEST(WorldDurableResidency, FlagOffMembershipIsInertAndEnablingInventoriesExistingChunksOnce) {
    DurableFixture fx(/*durable=*/false);
    ASSERT_TRUE(fx.created) << fx.error;
    const auto before = fx.world->region_digest_membership_updates();
    const auto chunk = std::make_shared<Chunk>(IVec3(128, 0, 128));
    chunk->set_state(ChunkState::Ready);
    chunk->heightmap_data = {3.0f};
    ASSERT_TRUE(fx.world->adopt_streamed_chunk(chunk));
    EXPECT_EQ(fx.world->region_digest_membership_updates(), before);
    fx.world->set_durable_residency(true);
    EXPECT_EQ(fx.world->region_digest_inputs(4, 4).live, std::vector{chunk});
    const auto enabled = fx.world->region_digest_membership_updates();
    fx.world->set_durable_residency(true);
    EXPECT_EQ(fx.world->region_digest_membership_updates(), enabled);
    fx.world->remember_durable_chunks({chunk->get_coords()});
    const auto remembered = fx.world->region_digest_membership_updates();
    fx.world->remember_durable_chunks({chunk->get_coords()});
    EXPECT_EQ(fx.world->region_digest_membership_updates(), remembered);
    fx.world->set_durable_residency(false);
    EXPECT_TRUE(fx.world->region_digest_inputs(4, 4).live.empty());
}

TEST(WorldDurableResidency, DurableSchedulerPrefixRespectsZeroAndPartialHeadroom) {
    for (const std::size_t headroom : {0u, 1u}) {
        SCOPED_TRACE(headroom);
        DurableFixture fx(/*durable=*/true);
        ASSERT_TRUE(fx.created) << fx.error;
        fx.world->clear_world(fx.physics);
        constexpr std::size_t cap = 8192; // Shipping resident admission limit.
        ASSERT_GT(cap, headroom);
        for (std::size_t i = 0; i < cap - headroom; ++i) {
            const auto chunk = std::make_shared<Chunk>(IVec3(1000 + static_cast<int>(i), 0, 0));
            // Loading placeholders do not schedule meshes or colliders in this admission test.
            chunk->set_state(ChunkState::Loading);
            ASSERT_TRUE(fx.world->adopt_streamed_chunk(chunk));
        }
        std::vector<IVec3> durable;
        for (int z = -16; z <= 16; ++z)
            for (int x = -16; x <= 16; ++x)
                for (int y = -16; y <= 16; ++y)
                    durable.emplace_back(x, y, z);
        fx.world->remember_durable_chunks(durable);
        std::size_t loads = 0;
        fx.world->set_durable_chunk_loader(
            [&](const IVec3& coords) {
                ++loads;
                const auto chunk = std::make_shared<Chunk>(coords);
                chunk->heightmap_data = {8.0f};
                chunk->mark_sdf_loaded_or_edited();
                return chunk;
            },
            [](const std::string&) { ADD_FAILURE() << "unexpected durable read failure"; });
        fx.world->update(fx.session->GetRegistry(), {Vec3(0, 8, 0)}, nullptr);
        const auto interval = fx.world->get_last_streaming_budget_stats().update_interval_frames;
        // Admission is measured on the scheduled activation pass, not an intervening frame.
        for (int frame = 1; frame < interval &&
                            fx.world->get_last_streaming_budget_stats().generation_candidates == 0;
             ++frame)
            fx.world->update(fx.session->GetRegistry(), {Vec3(0, 8, 0)}, nullptr);
        const auto& stats = fx.world->get_last_streaming_budget_stats();
        ASSERT_GT(static_cast<std::size_t>(stats.generation_budget), headroom);
        ASSERT_GT(stats.generation_candidates, headroom);
        EXPECT_EQ(loads, headroom);
        EXPECT_LE(stats.active_chunks_after, cap);
    }
}

TEST(WorldDurableResidency, DurableRestorationReplacesOnlyEmptyPlaceholders) {
    for (const bool populated : {false, true}) {
        SCOPED_TRACE(populated);
        DurableFixture fx(/*durable=*/true);
        ASSERT_TRUE(fx.created) << fx.error;
        const IVec3 coords(128, 0, 128);
        const auto resident = std::make_shared<Chunk>(coords);
        if (populated)
            resident->heightmap_data = {7.0f};
        ASSERT_TRUE(fx.world->adopt_streamed_chunk(resident));
        const auto saved = std::make_shared<Chunk>(coords);
        saved->sdf_data.assign(17u * 17u * 17u, -3.0f);
        saved->mark_sdf_loaded_or_edited();
        int loads = 0;
        fx.world->set_durable_chunk_loader(
            [&](const IVec3& requested) {
                EXPECT_EQ(requested, coords);
                ++loads;
                return saved;
            },
            [](const std::string&) { ADD_FAILURE() << "unexpected durable read failure"; });
        fx.world->remember_durable_chunks({coords});
        ASSERT_TRUE(fx.world->restore_authoritative_chunk(coords));
        EXPECT_EQ(loads, populated ? 0 : 1);
        EXPECT_EQ(fx.world->find_streamed_chunk(coords), populated ? resident : saved);
        const auto grouped = fx.world->region_digest_inputs(4, 4);
        ASSERT_EQ(grouped.live.size(), 1u);
        EXPECT_EQ(grouped.live.front(), populated ? resident : saved);
    }
}

TEST(WorldDurableResidency, InvalidDurableRestorationPreservesPlaceholderAndStopsGeneration) {
    for (const int failure : {0, 1, 2, 3}) {
        SCOPED_TRACE(failure);
        DurableFixture fx(/*durable=*/true);
        ASSERT_TRUE(fx.created) << fx.error;
        const IVec3 coords(128, 0, 128);
        const auto placeholder = std::make_shared<Chunk>(coords);
        ASSERT_TRUE(fx.world->adopt_streamed_chunk(placeholder));
        int faults = 0;
        fx.world->set_durable_chunk_loader(
            [&](const IVec3&) -> std::shared_ptr<Chunk> {
                if (failure == 0)
                    return nullptr;
                if (failure == 1)
                    throw std::runtime_error("unreadable durable payload");
                const auto saved =
                    std::make_shared<Chunk>(failure == 2 ? IVec3(129, 0, 128) : coords);
                if (failure == 2)
                    saved->heightmap_data = {7.0f};
                return saved;
            },
            [&](const std::string&) { ++faults; });
        fx.world->remember_durable_chunks({coords});
        EXPECT_TRUE(fx.world->restore_authoritative_chunk(coords));
        EXPECT_EQ(faults, 1);
        EXPECT_EQ(fx.world->find_streamed_chunk(coords), placeholder);
        fx.world->dispatch_generation_jobs(std::vector<IVec3>{coords});
        fx.world->wait_for_streaming_jobs();
        EXPECT_EQ(fx.world->find_streamed_chunk(coords), placeholder);
        EXPECT_TRUE(placeholder->sdf_data.empty());
        EXPECT_EQ(faults, 1);
    }
}

TEST(WorldDurableResidency, EmptyCommittedLookupRefusesAuthority) {
    DurableFixture fx(/*durable=*/true);
    ASSERT_TRUE(fx.created) << fx.error;
    const IVec3 coords(128, 0, 128);
    int loads = 0;
    int faults = 0;
    fx.world->set_durable_chunk_loader(
        [&](const IVec3& requested) {
            ++loads;
            return std::make_shared<Chunk>(requested);
        },
        [&](const std::string&) { ++faults; });
    EXPECT_EQ(fx.world->find_authoritative_chunk(coords), nullptr);
    EXPECT_EQ(loads, 0);
    fx.world->remember_durable_chunks({coords});
    EXPECT_EQ(fx.world->find_authoritative_chunk(coords), nullptr);
    EXPECT_EQ(loads, 1);
    EXPECT_EQ(faults, 1);
    EXPECT_TRUE(fx.world->restore_authoritative_chunk(coords));
    EXPECT_EQ(loads, 1);
    EXPECT_EQ(fx.world->find_streamed_chunk(coords), nullptr);
}

TEST(WorldDurableResidency, UnreadableRemeshNeighbourRefusesEditBeforeSdfChange) {
    DurableFixture fx(/*durable=*/true);
    ASSERT_TRUE(fx.created) << fx.error;
    const IVec3 coords(128, 0, 128);
    const auto target = std::make_shared<Chunk>(coords);
    target->set_state(ChunkState::Ready);
    target->sdf_data.assign(std::size_t{17} * 17 * 17, -3.0f);
    target->mark_sdf_loaded_or_edited();
    ASSERT_TRUE(fx.world->adopt_streamed_chunk(target));
    const auto sdf_before = target->sdf_data;
    const auto revision_before = target->voxel_revision();
    // A 2 m edit at the chunk centre touches only the target; the remesh band spans two
    // columns on each side, so every durable member below lies outside the edit sphere.
    std::vector<IVec3> neighbours;
    for (int y = -512; y <= 512; ++y)
        neighbours.emplace_back(coords.x + 2, y, coords.z);
    fx.world->remember_durable_chunks(neighbours);
    int loads = 0;
    int faults = 0;
    fx.world->set_durable_chunk_loader(
        [&](const IVec3& requested) -> std::shared_ptr<Chunk> {
            EXPECT_EQ(requested.x, coords.x + 2);
            ++loads;
            throw std::runtime_error("unreadable durable payload");
        },
        [&](const std::string&) { ++faults; });
    const Vec3 centre(static_cast<float>(coords.x * Luminumbra::CHUNK_SIZE_X) + 8.0f,
                      static_cast<float>(coords.y * Luminumbra::CHUNK_SIZE_Y) + 8.0f,
                      static_cast<float>(coords.z * Luminumbra::CHUNK_SIZE_Z) + 8.0f);
    EXPECT_EQ(fx.world->EditTerrainVoxel(centre, 2.0f, false, fx.physics), 0);
    EXPECT_EQ(loads, 1);
    EXPECT_EQ(faults, 1);
    EXPECT_EQ(target->sdf_data, sdf_before);
    EXPECT_EQ(target->voxel_revision(), revision_before);
    EXPECT_FALSE(target->is_voxel_data_dirty());
    EXPECT_EQ(fx.world->find_streamed_chunk(coords), target);
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

TEST(WorldDurableResidency, WaterAuthoritySynchronizationVisitsOnlyDirtyInputs) {
    DurableFixture fx(/*durable=*/true);
    ASSERT_TRUE(fx.created) << fx.error;
    fx.world->sync_water_authority();
    const auto before = fx.world->water_authority_input_items();
    const auto chunk = std::make_shared<Chunk>(IVec3(128, 0, 128));
    chunk->heightmap_data = {3.0f};
    chunk->mark_water_state_dirty(); // A pre-adoption mutation must also be enrolled.
    ASSERT_TRUE(fx.world->adopt_streamed_chunk(chunk));
    for (int i = 0; i < 100; ++i)
        chunk->mark_water_state_dirty();
    fx.world->sync_water_authority();
    EXPECT_EQ(fx.world->water_authority_input_items() - before, 1u);
    EXPECT_TRUE(chunk->has_water_authority());
    EXPECT_FALSE(chunk->is_water_state_dirty());
    fx.world->sync_water_authority();
    EXPECT_EQ(fx.world->water_authority_input_items() - before, 1u);
    fx.world->clear_world(fx.physics);
    chunk->mark_water_state_dirty(); // Detached chunks cannot enqueue work in the old world.
    fx.world->sync_water_authority();
    EXPECT_EQ(fx.world->water_authority_input_items() - before, 1u);
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

TEST(WorldDurableResidency, RepeatedSuccessfulSavesLeaveSettledActivationElided) {
    DurableFixture fx(/*durable=*/true, /*active_regions=*/true);
    ASSERT_TRUE(fx.created) << fx.error;
    fx.world->debug_set_streaming_radius_cap(1);
    const auto anchor = fx.SpawnAnchor();
    fx.Settle(anchor, kSettleIterations);
    ASSERT_TRUE(fx.world->get_streaming_telemetry_stats().quiescent());
    ASSERT_EQ(fx.world->parking_status().parked_count, 0u);
    ASSERT_EQ(fx.world->parking_status().blocked_dirty, 0u);
    ASSERT_FALSE(fx.world->persistence_save_requested());
    const auto generation = fx.world->debug_dirty_generation();
    const auto counters = fx.world->activation_index_counters();

    for (int save = 0; save < 3; ++save) {
        SCOPED_TRACE(save);
        ASSERT_TRUE(fx.session->SaveWorldState());
        EXPECT_EQ(fx.world->debug_dirty_generation(), generation);
        for (int frame = 0; frame < 8; ++frame) {
            fx.Settle(anchor, 1);
            EXPECT_EQ(fx.world->get_last_streaming_budget_stats().scheduled_generation, 0u);
            EXPECT_EQ(fx.world->debug_dirty_generation(), generation);
        }
        EXPECT_EQ(fx.world->activation_index_counters().columns_scanned, counters.columns_scanned);
        EXPECT_EQ(fx.world->activation_index_counters().index_updates, counters.index_updates);
        EXPECT_EQ(fx.world->activation_index_counters().span_cache_hits, counters.span_cache_hits);
        EXPECT_EQ(fx.world->activation_index_counters().span_cache_misses,
                  counters.span_cache_misses);
    }
}

TEST(WorldDurableResidency, FailedSessionSaveRearmsDirtyChunkForRetry) {
    DurableFixture fx(/*durable=*/true, /*active_regions=*/true);
    ASSERT_TRUE(fx.created) << fx.error;
    fx.world->set_parked_bytes_limit(1);
    fx.Settle(fx.SpawnAnchor(), kSettleIterations);

    const auto edited = fx.EditAtSpawnAndFindEdited();
    ASSERT_TRUE(edited);
    ASSERT_TRUE(edited->is_voxel_data_dirty());
    const auto coords = edited->get_coords();
    const auto far_anchor = fx.FarAnchor();
    fx.Settle(far_anchor, kSettleIterations);
    ASSERT_EQ(fx.world->find_streamed_chunk(coords), edited);
    ASSERT_GE(fx.world->parking_status().blocked_dirty, 1u);
    ASSERT_TRUE(fx.world->persistence_save_requested());

    const auto before_failure = fx.world->debug_dirty_generation();
    Luminumbra::Persistence::WorldSaveService::set_before_active_regions_replace_for_testing(
        [] { return false; });
    const bool failed_save = fx.session->SaveWorldState();
    Luminumbra::Persistence::WorldSaveService::set_before_active_regions_replace_for_testing(
        nullptr);
    EXPECT_FALSE(failed_save);
    EXPECT_TRUE(edited->is_voxel_data_dirty());
    EXPECT_EQ(fx.session->GetPersistenceStatus().failures, 1u);
    EXPECT_EQ(fx.world->debug_dirty_generation(), before_failure + 1);
    fx.Settle(far_anchor, kSettleIterations);
    EXPECT_EQ(fx.world->find_streamed_chunk(coords), edited);
    EXPECT_EQ(fx.world->parking_status().parked_count, 0u);

    // An explicit retry rewrites the re-armed chunk and acknowledges it.
    const auto before_retry = fx.world->debug_dirty_generation();
    ASSERT_TRUE(fx.session->SaveWorldState());
    EXPECT_EQ(fx.world->debug_dirty_generation(), before_retry + 1);
    EXPECT_FALSE(edited->is_voxel_data_dirty());
    EXPECT_FALSE(fx.world->persistence_save_requested());
    fx.Settle(far_anchor, kSettleIterations);
    EXPECT_FALSE(fx.world->find_streamed_chunk(coords));
    EXPECT_FALSE(Contains(fx.world->snapshot_parked_chunks(), edited.get()));
    const auto inventory = fx.world->durable_chunk_inventory();
    EXPECT_NE(std::find(inventory.begin(), inventory.end(), coords), inventory.end());
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

// Temp root containing the dry preset and its worldgen dependencies.
// The root is unique per fixture instance; the destructor removes the tree.
class DryResidencyRoot {
public:
    DryResidencyRoot() {
        root_ = Luminumbra::TestSupport::CreateUniqueTempDirectory("dry_residency_");
        fs::create_directories(root_ / "worlds" / "atlas" / "presets");
        std::ofstream preset;
        preset.exceptions(std::ios::failbit | std::ios::badbit);
        preset.open(root_ / "worlds" / "atlas" / "presets" / "durable.json");
        preset << kResidencyPreset;
        preset.close();
        Luminumbra::TestSupport::CopyWorldgenTestData(LUMINUMBRA_SOURCE_ROOT, root_);
    }
    ~DryResidencyRoot() {
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
constexpr int kDryStreamRadiusCap = 1;
// Far anchor distance in chunks along X: well outside radius + 2 of the spawn anchor.
constexpr float kDryFarChunks = 40.0f;
// Activation runs every four updates. Three passes suffice for this small set:
// generate/restore, mesh and drain, then retry any eviction deferred by in-flight work.
// Each update explicitly drains the real streaming jobs, independent of worker timing.
constexpr int kDrySettleIterations = 12;

// One headless world per TEST. Member order: root outlives the session, the job
// system outlives the session, and the destructor tears the session down before
// shutting the job system down.
class DryDurableFixture {
public:
    explicit DryDurableFixture(bool durable, bool active_regions = false) {
        // CTest runs cases in separate processes; bound each small fixture's worker pool.
        jobs.startup(2);
        session = std::make_unique<GameSession>();
        // This fixture requires dry, regenerable water; weather rain is enabled by default.
        session->SetWeatherRainEnabled(false);
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
        world->debug_set_streaming_radius_cap(kDryStreamRadiusCap);
        if (!active_regions) {
            world->set_durable_residency(durable);
        }
        physics = session->GetPhysicsSystem();
        spawn = session->GetMetadata().spawnPoint;
    }
    ~DryDurableFixture() {
        session.reset();
        jobs.shutdown();
    }
    DryDurableFixture(const DryDurableFixture&) = delete;
    DryDurableFixture& operator=(const DryDurableFixture&) = delete;

    Vec3 AnchorAt(float x, float z) {
        return Vec3(x, world->GetTerrainHeightAt(x, z) + 2.0f, z);
    }
    Vec3 SpawnAnchor() {
        return AnchorAt(spawn.x, spawn.z);
    }
    Vec3 FarAnchor() {
        return AnchorAt(spawn.x + kDryFarChunks * static_cast<float>(Luminumbra::CHUNK_SIZE_X),
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

    DryResidencyRoot root;
    JobSystem jobs;
    std::unique_ptr<GameSession> session;
    SHIELD_WorldSystem* world = nullptr;
    Luminumbra::Systems::PhysicsSystem* physics = nullptr;
    Vec3 spawn{};
    bool created = false;
    std::string error;
};

TEST(WorldDurableResidency, DryTerrainParksTheEditAndDiscardsRegenerableChunks) {
    DryDurableFixture fx(/*durable=*/true);
    ASSERT_TRUE(fx.created) << fx.error;
    fx.Settle(fx.SpawnAnchor(), kDrySettleIterations);
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
    fx.Settle(fx.FarAnchor(), kDrySettleIterations);
    EXPECT_TRUE(Contains(fx.world->snapshot_parked_chunks(), edited.get()));
    for (const auto& chunk : regenerable)
        EXPECT_FALSE(fx.world->find_authoritative_chunk(chunk->get_coords()));
    // Water that moved makes a chunk authoritative; everything else is regenerable and erased.
    for (const auto& parked : fx.world->snapshot_parked_chunks()) {
        ASSERT_TRUE(parked);
        EXPECT_TRUE(parked->needs_authority());
    }
}

} // namespace
