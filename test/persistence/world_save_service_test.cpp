#include "gtest/gtest.h"

#include "../support/CalendarMetadataCases.h"
#include "../support/UniqueTempPath.h"

#include "persistence/WorldPersistenceRoundtrip.h"
#include "persistence/WorldSaveService.h"
#include "systems/SHIELD_WorldSystem.h"
#include "world/MarchingCubes.h"
#include "world/WorldStreamingState.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Luminumbra::Chunk;
using Luminumbra::ChunkState;
using Luminumbra::IVec3;
using Luminumbra::Vec2;
using Luminumbra::Vec3;
using Luminumbra::WorldStreamingState;
using Luminumbra::Persistence::WorldSaveService;

std::filesystem::path MakeTempSaveDir(const std::string& tag) {
    const std::filesystem::path dir = Luminumbra::TestSupport::CreateUniqueTempDirectory(
        "luminumbra_world_save_service_" + tag + "_");

    return dir;
}

struct TempSaveDir {
    explicit TempSaveDir(const std::string& tag)
        : path(MakeTempSaveDir(tag)) {}
    ~TempSaveDir() {
        std::error_code remove_error;
        std::filesystem::remove_all(path, remove_error);
    }

    std::filesystem::path path;
};

std::shared_ptr<Chunk> AddFixtureChunk(WorldStreamingState& state,
                                       const IVec3& coords,
                                       ChunkState chunk_state,
                                       Luminumbra::u32 salt) {
    auto chunk = state.get_or_create_chunk(coords);
    chunk->set_state(chunk_state);
    chunk->sdf_data = {-1.5f + static_cast<float>(salt), -0.25f, 0.5f, 1.25f};
    chunk->heightmap_data = {7.0f + static_cast<float>(salt), 8.5f};
    chunk->mesh_vertices = {{Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), salt + 1u},
                            {Vec3(1.0f, 1.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), salt + 2u},
                            {Vec3(0.0f, 1.0f, 1.0f), Vec3(0.0f, 1.0f, 0.0f), salt + 3u}};
    chunk->mesh_indices = {0u, 1u, 2u};
    chunk->water_level_data = {2.0f, 2.25f};
    chunk->water_flow_data = {Vec2(0.25f, -0.125f)};
    chunk->water_sim_terrain_height = {1.0f, 1.5f};
    chunk->has_collision.store(true, std::memory_order_release);
    chunk->current_lod.store(static_cast<int>(salt % 3u), std::memory_order_release);
    chunk->mesh_version.store(5u + salt, std::memory_order_release);
    chunk->has_water_sim.store(true, std::memory_order_release);
    chunk->max_water_delta_last_tick = 0.0625f * static_cast<float>(salt + 1u);
    chunk->ticks_below_threshold = static_cast<int>(salt);
    return chunk;
}

void PopulateFixtureWorld(WorldStreamingState& state) {
    AddFixtureChunk(state, IVec3(0, 0, 0), ChunkState::Ready, 1u);
    AddFixtureChunk(state, IVec3(2, -1, 3), ChunkState::Idle, 2u);
    AddFixtureChunk(state, IVec3(-1, 1, -2), ChunkState::Meshing, 3u);
}

} // namespace

TEST(WorldSaveService, SaveThenLoadRoundTripPreservesWorldHash) {
    TempSaveDir save_dir("roundtrip");
    WorldSaveService service;

    WorldStreamingState original;
    PopulateFixtureWorld(original);
    const std::string original_hash = service.world_hash(original);
    ASSERT_FALSE(original_hash.empty());

    std::vector<std::string> save_errors;
    ASSERT_TRUE(service.save_world(original, save_dir.path, &save_errors));
    EXPECT_TRUE(save_errors.empty());
    // Persistence v2: the writer emits the LMR1 region container,
    // never the legacy v1 single snapshot.
    EXPECT_FALSE(std::filesystem::exists(WorldSaveService::world_state_path(save_dir.path)));
    EXPECT_TRUE(std::filesystem::exists(WorldSaveService::world_manifest_path(save_dir.path)));
    EXPECT_TRUE(std::filesystem::exists(WorldSaveService::region_file_path(save_dir.path, 0, 0)));

    WorldStreamingState restored;
    std::vector<std::string> load_errors;
    ASSERT_TRUE(service.load_world(restored, save_dir.path, load_errors));
    EXPECT_TRUE(load_errors.empty());

    EXPECT_EQ(restored.size(), original.size());
    EXPECT_EQ(service.world_hash(restored), original_hash);
}

TEST(WorldSaveService, LoadFromEmptySaveDirIsCleanMissWithoutErrors) {
    TempSaveDir save_dir("fresh");
    WorldSaveService service;

    WorldStreamingState state;
    std::vector<std::string> errors;
    EXPECT_FALSE(service.load_world(state, save_dir.path, errors));
    EXPECT_TRUE(errors.empty());
    EXPECT_TRUE(state.empty());
}

TEST(WorldSaveService, LoadFromMissingSaveDirIsCleanMissWithoutErrors) {
    const std::filesystem::path missing_dir =
        std::filesystem::temp_directory_path() / "luminumbra_world_save_service_does_not_exist";
    WorldSaveService service;

    WorldStreamingState state;
    std::vector<std::string> errors;
    EXPECT_FALSE(service.load_world(state, missing_dir, errors));
    EXPECT_TRUE(errors.empty());
}

TEST(ChunkDirtyTracking, DirectVoxelWritePlusMarkIsVisibleThroughStreamingState) {
    WorldStreamingState state;
    PopulateFixtureWorld(state);

    for (const auto& chunk : state.snapshot_chunks()) {
        EXPECT_FALSE(chunk->is_voxel_data_dirty());
    }
    EXPECT_TRUE(state.dirty_chunk_ids().empty());

    auto edited = state.find_chunk(IVec3(2, -1, 3));
    ASSERT_NE(edited, nullptr);
    edited->sdf_data[0] = -42.0f; // direct voxel mutation post-generation
    edited->mark_voxel_data_dirty();

    EXPECT_TRUE(edited->is_voxel_data_dirty());
    const std::vector<Luminumbra::ChunkID> dirty_ids = state.dirty_chunk_ids();
    ASSERT_EQ(dirty_ids.size(), 1u);
    EXPECT_EQ(dirty_ids.front(), edited->get_id());

    edited->clear_voxel_data_dirty();
    EXPECT_FALSE(edited->is_voxel_data_dirty());
    EXPECT_TRUE(state.dirty_chunk_ids().empty());
}

TEST(ChunkDirtyTracking, SaveDirtyChunksWritesSnapshotAndClearsFlags) {
    TempSaveDir save_dir("dirty");
    WorldSaveService service;

    WorldStreamingState state;
    PopulateFixtureWorld(state);
    auto edited = state.find_chunk(IVec3(0, 0, 0));
    ASSERT_NE(edited, nullptr);
    edited->sdf_data[1] = 9.5f;
    edited->mark_voxel_data_dirty();

    std::vector<std::string> errors;
    const auto report = service.save_dirty_chunks(state, save_dir.path, &errors);
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(report.chunks_total, 3u);
    EXPECT_EQ(report.chunks_dirty, 1u);
    EXPECT_TRUE(report.saved);
    EXPECT_GE(report.regions_written, 1u);
    EXPECT_TRUE(std::filesystem::exists(WorldSaveService::region_file_path(save_dir.path, 0, 0)));
    EXPECT_FALSE(edited->is_voxel_data_dirty());
    EXPECT_TRUE(state.dirty_chunk_ids().empty());

    // Second pass with nothing dirty performs no save.
    const auto clean_report = service.save_dirty_chunks(state, save_dir.path, &errors);
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(clean_report.chunks_total, 3u);
    EXPECT_EQ(clean_report.chunks_dirty, 0u);
    EXPECT_FALSE(clean_report.saved);
}

TEST(ChunkDirtyTracking, SaveDirtyChunksWithoutDirtyChunksWritesNothing) {
    TempSaveDir save_dir("clean");
    WorldSaveService service;

    WorldStreamingState state;
    PopulateFixtureWorld(state);

    std::vector<std::string> errors;
    const auto report = service.save_dirty_chunks(state, save_dir.path, &errors);
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(report.chunks_total, 3u);
    EXPECT_EQ(report.chunks_dirty, 0u);
    EXPECT_FALSE(report.saved);
    EXPECT_FALSE(std::filesystem::exists(WorldSaveService::world_state_path(save_dir.path)));
    EXPECT_FALSE(std::filesystem::exists(WorldSaveService::region_directory(save_dir.path)));
}

TEST(ChunkDirtyTracking, GenerationAndMeshingLeaveChunkClean) {
    Luminumbra::Systems::TerrainGenParams params;
    params.base_amplitude = 0.0f;
    params.height_offset = 8.0f;
    params.caves_enabled = false;
    const Luminumbra::Systems::SHIELD_WorldSystem world_system(nullptr, nullptr, params, 1337);

    Chunk chunk(IVec3(0, 0, 0));
    world_system.GenerateChunkData(chunk);
    EXPECT_FALSE(chunk.is_voxel_data_dirty()) << "generation must leave the chunk clean";

    Luminumbra::World::MarchingCubes::PolygoniseTerrain(world_system, chunk, 0.0f, 1);
    EXPECT_FALSE(chunk.is_voxel_data_dirty()) << "meshing must not mark voxel data dirty";

    // Regeneration discards unsaved edits, so it clears the flag again.
    chunk.sdf_data[0] = -100.0f;
    chunk.mark_voxel_data_dirty();
    world_system.GenerateChunkData(chunk);
    EXPECT_FALSE(chunk.is_voxel_data_dirty()) << "regeneration must reset the dirty flag";
}

TEST(WorldSaveService, ClockMetadataValidationPrecedesEveryWrite) {
    TempSaveDir save_dir("clock_metadata");
    const auto metadata_path = save_dir.path / "world_info.json";
    const auto read = [&]() {
        std::ifstream file(metadata_path, std::ios::binary);
        std::ostringstream bytes;
        bytes << file.rdbuf();
        return bytes.str();
    };
    std::vector<std::string> errors;
    EXPECT_FALSE(WorldSaveService::save_metadata(
        R"({"container_version":2,"simulationTick":-1})", save_dir.path, &errors));
    EXPECT_FALSE(std::filesystem::exists(metadata_path));
    nlohmann::json metadata = {{"container_version", 2}};
    Luminumbra::world::WorldClock(123, {12000, 4}).write_metadata(metadata);
    const auto valid = metadata.dump(4) + "\n";
    ASSERT_TRUE(WorldSaveService::save_metadata(valid, save_dir.path));
    EXPECT_EQ(read(), valid);
    EXPECT_FALSE(WorldSaveService::save_metadata(R"({"container_version":2})", save_dir.path));
    EXPECT_EQ(read(), valid);
    metadata["calendar"]["daysPerYear"] = 0;
    EXPECT_FALSE(WorldSaveService::save_metadata(metadata.dump(), save_dir.path));
    EXPECT_EQ(read(), valid);
    // A pre-existing corrupt clock blocks chunk, entity and metadata writers.
    {
        std::ofstream output(metadata_path, std::ios::binary | std::ios::trunc);
        output << metadata.dump();
    }
    const auto damaged = read();
    WorldStreamingState state;
    PopulateFixtureWorld(state);
    EXPECT_FALSE(WorldSaveService{}.save_world(state, save_dir.path));
    EXPECT_FALSE(WorldSaveService::save_metadata(valid, save_dir.path));
    EXPECT_FALSE(std::filesystem::exists(save_dir.path / "chunks"));
    EXPECT_EQ(read(), damaged);
}

TEST(WorldSaveService, LegacyMetadataWriterPreservesOpaqueBytesAndMissingDirectoryFailure) {
    TempSaveDir root("legacy_metadata");
    const auto missing = root.path / "missing";
    EXPECT_FALSE(WorldSaveService::save_metadata(R"({"container_version":2})", missing));
    EXPECT_FALSE(std::filesystem::exists(missing));
    for (const std::string bytes : {"{", "[]", "null", "opaque legacy input"}) {
        SCOPED_TRACE(bytes);
        TempSaveDir destination("opaque_metadata");
        ASSERT_TRUE(WorldSaveService::save_metadata(bytes, destination.path));
        std::ifstream input(destination.path / "world_info.json", std::ios::binary);
        std::ostringstream saved;
        saved << input.rdbuf();
        EXPECT_EQ(saved.str(), bytes);
    }
}

TEST(WorldSaveService, DirectLegacySnapshotsKeepMetadataAboveCatalogSizeLimit) {
    TempSaveDir destination("large_metadata");
    const nlohmann::json metadata = {{"container_version", 2},
                                     {"extension", std::string(1024 * 1024, 'x')}};
    const auto bytes = metadata.dump();
    ASSERT_TRUE(WorldSaveService::save_metadata(bytes, destination.path));
    WorldStreamingState original, loaded;
    PopulateFixtureWorld(original);
    ASSERT_TRUE(WorldSaveService{}.save_world(original, destination.path));
    std::vector<std::string> errors;
    ASSERT_TRUE(WorldSaveService{}.load_world(loaded, destination.path, errors));
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(WorldSaveService{}.world_hash(loaded), WorldSaveService{}.world_hash(original));
    EXPECT_TRUE(WorldSaveService::save_metadata(bytes, destination.path));
}

TEST(WorldSaveServiceTest, ChunkVectorRegionDigestUsesV2AuthorityAndDurableMerge) {
    using Luminumbra::Persistence::RegionDigestInputs;
    const TempSaveDir root("vector_digest");
    WorldStreamingState durable;
    const auto stored = AddFixtureChunk(durable, IVec3(0, 0, 0), ChunkState::Idle, 1u);
    const auto retained = AddFixtureChunk(durable, IVec3(1, 0, 0), ChunkState::Idle, 2u);
    stored->mark_voxel_data_dirty();
    retained->mark_voxel_data_dirty();
    ASSERT_TRUE(WorldSaveService{}.save_world(durable, root.path));

    WorldStreamingState resident;
    const auto live = AddFixtureChunk(resident, IVec3(0, 0, 0), ChunkState::Idle, 3u);
    const auto pristine = AddFixtureChunk(resident, IVec3(1, 0, 0), ChunkState::Idle, 4u);
    const auto loading = AddFixtureChunk(resident, IVec3(2, 0, 0), ChunkState::Loading, 5u);
    const auto outside = AddFixtureChunk(resident, IVec3(32, 0, 0), ChunkState::Idle, 6u);
    live->mark_voxel_data_dirty();
    loading->mark_voxel_data_dirty();
    outside->mark_voxel_data_dirty();
    const std::vector<std::shared_ptr<Chunk>> chunks{nullptr, pristine, loading, outside, live};

    WorldStreamingState expected;
    ASSERT_TRUE(expected.insert_chunk(live));
    ASSERT_TRUE(expected.insert_chunk(retained));
    const auto expected_digest =
        std::stoull(Luminumbra::Persistence::ComputeRegionContentHash(expected), nullptr, 16);
    EXPECT_EQ(WorldSaveService::region_simulation_digest(root.path, {0, 0}, chunks),
              expected_digest);
    EXPECT_EQ(WorldSaveService::region_simulation_digest(
                  root.path, {0, 0}, RegionDigestInputs{chunks, {}}),
              expected_digest);

    const std::vector<std::shared_ptr<Chunk>> empty;
    const auto durable_digest =
        std::stoull(Luminumbra::Persistence::ComputeRegionContentHash(durable), nullptr, 16);
    EXPECT_EQ(WorldSaveService::region_simulation_digest(root.path, {0, 0}, empty), durable_digest);
    EXPECT_EQ(WorldSaveService::region_simulation_digest(root.path, {0, 0}, {}), durable_digest);
    EXPECT_EQ(WorldSaveService::region_simulation_digest(root.path, {0, 0}, {live}),
              expected_digest);
    const std::vector<std::shared_ptr<Chunk>> only_live{live};
    const auto live_digest = WorldSaveService::region_simulation_digest({}, {0, 0}, only_live);
    EXPECT_EQ(WorldSaveService::region_simulation_digest({}, {0, 0}, chunks), live_digest);
    EXPECT_EQ(WorldSaveService::region_simulation_digest(root.path / "missing", {0, 0}, chunks),
              live_digest);
}

TEST(WorldSaveServiceTest, EditedLoadingRecordKeepsFrozenDigestAcrossResidency) {
    using Luminumbra::Persistence::RegionDigestInputs;
    using namespace Luminumbra::world;
    const TempSaveDir root("loading_frozen_digest");
    const RegionKey key{10, -3};
    WorldStreamingState resident;
    auto chunk = resident.get_or_create_chunk({key.x * 32, 0, key.z * 32});
    chunk->heightmap_data = {8.0f};
    chunk->sdf_data = {-1.0f, 1.0f};
    chunk->material_data = {2};
    chunk->mark_voxel_data_dirty();
    chunk->set_state(ChunkState::Loading);
    ASSERT_TRUE(WorldSaveService{}.save_world(resident, root.path));

    std::vector<std::shared_ptr<Chunk>> saved;
    ASSERT_TRUE(WorldSaveService::read_authoritative_region_chunks(
        WorldSaveService::region_file_path(root.path, key.x, key.z),
        key.x * 32,
        key.x * 32 + 31,
        key.z * 32,
        key.z * 32 + 31,
        saved));
    ASSERT_EQ(saved.size(), 1u);
    ASSERT_EQ(saved.front()->get_state(), ChunkState::Loading);
    ASSERT_EQ(saved.front()->sdf_provenance(), Luminumbra::ChunkSdfProvenance::LoadedOrEdited);

    const auto freeze = [key](const std::filesystem::path& save, const RegionDigestInputs& inputs) {
        ActiveRegionLedger ledger({0, 1, 1});
        ledger.mark_edited(key, WorldClock{});
        ledger.schedule(WorldClock(1), std::span<const Vec3>{});
        const std::array work{RegionWork{key, 1, 0, 0, 0}};
        const auto provider = [&](RegionKey region) {
            return WorldSaveService::region_simulation_digest(save, region, inputs);
        };
        ledger.schedule(WorldClock(2), std::span<const Vec3>{}, work, provider);
        ledger.schedule(WorldClock(3), std::span<const Vec3>{}, work, provider);
        EXPECT_EQ(ledger.records().at(key).state, RegionState::Frozen);
        return ledger.records().at(key).frozen_digest;
    };
    chunk->set_state(ChunkState::Idle);
    const auto settled = freeze({}, RegionDigestInputs{resident.snapshot_chunks(), {}});
    ASSERT_NE(settled, freeze({}, RegionDigestInputs{}));
    EXPECT_EQ(freeze(root.path, RegionDigestInputs{resident.snapshot_chunks(), {}}), settled);
    EXPECT_EQ(freeze({}, RegionDigestInputs{{}, resident.snapshot_chunks()}), settled);

    // Incomplete live/parked content must fall back to the edited durable Loading record.
    auto incomplete = std::make_shared<Chunk>(chunk->get_coords());
    incomplete->heightmap_data = {99.0f};
    incomplete->mark_voxel_data_dirty();
    incomplete->set_state(ChunkState::Loading);
    EXPECT_EQ(freeze({}, RegionDigestInputs{{incomplete}, {}}), freeze({}, RegionDigestInputs{}));
    EXPECT_EQ(freeze({}, RegionDigestInputs{{}, {incomplete}}), freeze({}, RegionDigestInputs{}));
    EXPECT_EQ(freeze(root.path, RegionDigestInputs{{incomplete}, {incomplete}}), settled);

    resident.clear();
    chunk.reset();
    saved.clear();
    EXPECT_EQ(freeze(root.path, RegionDigestInputs{}), settled);
}

TEST(WorldSaveServiceTest, RegionDigestAuthorityAndLifecycleMatrix) {
    using Luminumbra::ChunkSdfProvenance;
    using Luminumbra::Persistence::RegionDigestInputs;
    const auto empty = WorldSaveService::region_simulation_digest({}, {0, 0}, {});
    for (const auto lifecycle : {ChunkState::Unloaded,
                                 ChunkState::Loading,
                                 ChunkState::Idle,
                                 ChunkState::Meshing,
                                 ChunkState::Ready,
                                 ChunkState::Unloading}) {
        for (const bool edited : {false, true}) {
            SCOPED_TRACE(static_cast<int>(lifecycle));
            SCOPED_TRACE(edited);
            const TempSaveDir root("digest_lifecycle");
            WorldStreamingState resident;
            const auto chunk = resident.get_or_create_chunk({0, 0, 0});
            chunk->heightmap_data = {8.0f};
            if (edited)
                chunk->mark_voxel_data_dirty();
            chunk->set_state(ChunkState::Idle);
            const auto settled = WorldSaveService::region_simulation_digest(
                {}, {0, 0}, RegionDigestInputs{{chunk}, {}});
            if (edited)
                ASSERT_NE(settled, empty);
            else
                ASSERT_EQ(settled, empty);
            chunk->set_state(lifecycle);
            const auto in_memory = lifecycle == ChunkState::Loading ? empty : settled;
            EXPECT_EQ(WorldSaveService::region_simulation_digest(
                          {}, {0, 0}, RegionDigestInputs{{chunk}, {}}),
                      in_memory);
            EXPECT_EQ(WorldSaveService::region_simulation_digest(
                          {}, {0, 0}, RegionDigestInputs{{}, {chunk}}),
                      in_memory);

            ASSERT_TRUE(WorldSaveService{}.save_world(resident, root.path));
            WorldStreamingState loaded;
            std::vector<std::string> errors;
            ASSERT_TRUE(WorldSaveService{}.load_world(loaded, root.path, errors));
            const auto saved = loaded.snapshot_chunks();
            ASSERT_EQ(saved.size(), 1u);
            EXPECT_EQ(saved.front()->get_state(), lifecycle);
            EXPECT_EQ(saved.front()->sdf_provenance(),
                      edited ? ChunkSdfProvenance::LoadedOrEdited
                             : ChunkSdfProvenance::GeneratedCurrentParams);
            // docs/active-regions.md: durable pristine records are regenerable caches.
            EXPECT_EQ(WorldSaveService::region_simulation_digest(root.path, {0, 0}, {}), settled);
        }
    }
}

TEST(WorldSaveServiceTest, PublicRegionDigestRejectsUnsupportedKeysBeforeDurableRead) {
    using Luminumbra::world::RegionKey;
    const Luminumbra::Persistence::RegionDigestInputs inputs{};
    const TempSaveDir root("invalid_digest_key");
    const auto missing_save = root.path / "missing";
    for (const RegionKey key : {RegionKey{std::numeric_limits<std::int32_t>::min(), 0},
                                RegionKey{std::numeric_limits<std::int32_t>::max(), 0},
                                RegionKey{0, std::numeric_limits<std::int32_t>::min()},
                                RegionKey{0, std::numeric_limits<std::int32_t>::max()},
                                RegionKey{-32769, 0},
                                RegionKey{32768, 0},
                                RegionKey{0, -32769},
                                RegionKey{0, 32768}}) {
        EXPECT_THROW((void)WorldSaveService::region_simulation_digest({}, key, inputs),
                     std::invalid_argument);
        EXPECT_THROW((void)WorldSaveService::region_simulation_digest(missing_save, key, inputs),
                     std::invalid_argument);
        EXPECT_THROW((void)WorldSaveService::region_simulation_digest({}, key, inputs.live),
                     std::invalid_argument);
        EXPECT_THROW(
            (void)WorldSaveService::region_simulation_digest(missing_save, key, inputs.live),
            std::invalid_argument);
    }
    const auto empty_digest = WorldSaveService::region_simulation_digest({}, {0, 0}, inputs);
    for (const RegionKey key : {RegionKey{-32768, -32768},
                                RegionKey{-32768, 32767},
                                RegionKey{32767, -32768},
                                RegionKey{32767, 32767},
                                RegionKey{0, 0}}) {
        EXPECT_EQ(WorldSaveService::region_simulation_digest(missing_save, key, inputs),
                  empty_digest);
        WorldStreamingState region;
        const auto chunk =
            AddFixtureChunk(region, IVec3(key.x * 32, 0, key.z * 32), ChunkState::Idle, 1u);
        chunk->mark_voxel_data_dirty();
        const Luminumbra::Persistence::RegionDigestInputs edited_inputs{{chunk}, {}};
        const auto expected_digest =
            std::stoull(Luminumbra::Persistence::ComputeRegionContentHash(region), nullptr, 16);
        EXPECT_EQ(WorldSaveService::region_simulation_digest({}, key, edited_inputs),
                  expected_digest);
        EXPECT_EQ(WorldSaveService::region_simulation_digest(missing_save, key, edited_inputs),
                  expected_digest);
        EXPECT_EQ(WorldSaveService::region_simulation_digest({}, key, edited_inputs.live),
                  expected_digest);
        EXPECT_EQ(WorldSaveService::region_simulation_digest(missing_save, key, edited_inputs.live),
                  expected_digest);
    }
    EXPECT_FALSE(std::filesystem::exists(missing_save));
}

TEST(WorldSaveService, BothCalendarKindsAreValidatedBeforeEveryWriter) {
    for (const auto& patch : Luminumbra::TestSupport::InvalidCalendarMetadata()) {
        SCOPED_TRACE(patch.dump());
        TempSaveDir save_dir("calendar_matrix");
        auto metadata = nlohmann::json{{"container_version", 2}};
        metadata.update(patch);
        EXPECT_FALSE(WorldSaveService::save_metadata(metadata.dump(), save_dir.path));
        EXPECT_FALSE(std::filesystem::exists(save_dir.path / "world_info.json"));
        Luminumbra::world::ActiveRegionLedger ledger;
        EXPECT_FALSE(WorldSaveService::save_metadata_and_active_regions(
            metadata.dump(), ledger, save_dir.path));
        std::ofstream(save_dir.path / "world_info.json", std::ios::binary) << metadata.dump();
        WorldStreamingState state;
        PopulateFixtureWorld(state);
        const auto before = metadata.dump();
        const auto read = [&]() {
            std::ifstream input(save_dir.path / "world_info.json", std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(input), {});
        };
        EXPECT_FALSE(WorldSaveService{}.save_world(state, save_dir.path));
        EXPECT_FALSE(WorldSaveService{}.save_dirty_chunks(state, save_dir.path).saved);
        EXPECT_FALSE(WorldSaveService::save_plant_entities({}, save_dir.path));
        EXPECT_FALSE(WorldSaveService::upsert_container_records(
            WorldSaveService::region_file_path(save_dir.path, 0, 0), {}));
        EXPECT_FALSE(WorldSaveService::save_active_regions(
            ledger, Luminumbra::world::WorldClock{}, save_dir.path));
        EXPECT_FALSE(WorldSaveService::save_metadata(R"({"container_version":2})", save_dir.path));
        EXPECT_FALSE(WorldSaveService::save_metadata_and_active_regions(
            R"({"container_version":2,"simulationTick":0})", ledger, save_dir.path));
        EXPECT_EQ(read(), before);
        EXPECT_FALSE(std::filesystem::exists(save_dir.path / "chunks"));
    }
}

TEST(WorldSaveService, SupportedCalendarWritesNormalizeOnlyOnSuccessfulExplicitSave) {
    for (const auto* key : {"calendar", "renderCalendar"}) {
        TempSaveDir save_dir("calendar_normalize");
        const auto path = save_dir.path / "world_info.json";
        auto metadata = nlohmann::json{{"container_version", 2}};
        metadata[key] = key == std::string("calendar")
                            ? nlohmann::json{{"dayLengthTicks", 36000}, {"daysPerYear", 3}}
                            : Luminumbra::TestSupport::DefaultCalendarMetadata();
        const std::string before = metadata.dump();
        std::ofstream(path, std::ios::binary) << before;
        const auto read = [&]() {
            std::ifstream input(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(input), {});
        };
        Luminumbra::world::WorldClock clock;
        bool required = false;
        ASSERT_TRUE(WorldSaveService::read_clock_metadata(save_dir.path, clock, required));
        EXPECT_EQ(required, key == std::string("calendar"));
        EXPECT_EQ(read(), before);
        WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
        EXPECT_FALSE(WorldSaveService::save_metadata(before, save_dir.path));
        EXPECT_EQ(read(), before);
        ASSERT_TRUE(WorldSaveService::save_metadata(before, save_dir.path));
        const auto written = nlohmann::json::parse(read());
        EXPECT_EQ(written.at(key).at("version"), 2);
        EXPECT_EQ(written.at(key).size(), 6u);
        if (required) {
            EXPECT_EQ(written.at(key).at("dayLengthTicks"), 36000);
            EXPECT_EQ(written.at(key).at("daylightNumerator"), 1);
            EXPECT_EQ(written.at(key).at("daylightDenominator"), 2);
        } else {
            EXPECT_FALSE(written.contains("simulationTick"));
            EXPECT_FALSE(written.contains("calendar"));
        }
    }
}
