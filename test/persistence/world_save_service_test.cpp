#include "../fixtures/TerrainFixtureParams.h"
#include "gtest/gtest.h"

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
    Luminumbra::Systems::TerrainGenParams params = Luminumbra::Test::SyntheticTerrainParams();
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

TEST(WorldSaveService, LedgerClockMismatchRefusesEmptyAndPopulatedSavesBeforeWriting) {
    using Luminumbra::world::ActiveRegionLedger;
    using Luminumbra::world::WorldClock;
    for (const bool populated : {false, true}) {
        SCOPED_TRACE(populated);
        TempSaveDir destination("ledger_clock");
        const auto read = [](const std::filesystem::path& path) {
            std::ifstream file(path, std::ios::binary);
            std::ostringstream bytes;
            bytes << file.rdbuf();
            return bytes.str();
        };
        ActiveRegionLedger ledger({}, WorldClock(1));
        if (populated)
            ledger.mark_edited({0, 0}, WorldClock(1));
        nlohmann::json metadata = {{"container_version", 2}};
        WorldClock(1).write_metadata(metadata);
        const auto original_ledger = ledger.encode();
        // Exercise refusal before the no-file/empty-ledger shortcut as well.
        for (const std::uint64_t tick : {0u, 2u}) {
            WorldClock(tick).write_metadata(metadata);
            EXPECT_FALSE(
                WorldSaveService::save_active_regions(ledger, WorldClock(tick), destination.path));
            EXPECT_FALSE(WorldSaveService::save_metadata_and_active_regions(
                metadata.dump(), ledger, destination.path));
            EXPECT_EQ(ledger.encode(), original_ledger);
            EXPECT_FALSE(std::filesystem::exists(destination.path / "world_info.json"));
            EXPECT_FALSE(
                std::filesystem::exists(WorldSaveService::active_regions_path(destination.path)));
        }
        if (!populated) {
            EXPECT_TRUE(
                WorldSaveService::save_active_regions(ledger, WorldClock(1), destination.path));
            EXPECT_FALSE(
                std::filesystem::exists(WorldSaveService::active_regions_path(destination.path)));
        }
        // An anchor-only ledger must be installed even without region records.
        ledger.set_local_anchor({8, 0, 8});
        WorldClock(1).write_metadata(metadata);
        ASSERT_TRUE(WorldSaveService::save_metadata_and_active_regions(
            metadata.dump(), ledger, destination.path));
        const auto saved_metadata = read(destination.path / "world_info.json");
        const auto saved_ledger = read(WorldSaveService::active_regions_path(destination.path));
        for (const std::uint64_t tick : {0u, 2u}) {
            WorldClock(tick).write_metadata(metadata);
            EXPECT_FALSE(WorldSaveService::save_metadata_and_active_regions(
                metadata.dump(), ledger, destination.path));
            EXPECT_FALSE(
                WorldSaveService::save_active_regions(ledger, WorldClock(tick), destination.path));
            EXPECT_EQ(read(destination.path / "world_info.json"), saved_metadata);
            EXPECT_EQ(read(WorldSaveService::active_regions_path(destination.path)), saved_ledger);
            EXPECT_EQ(ledger.encode(), saved_ledger);
        }
        // Recovery may acknowledge a newer clock without changing the canonical tick.
        ledger.restore_clock(WorldClock(2));
        WorldClock(2).write_metadata(metadata);
        ASSERT_TRUE(WorldSaveService::save_metadata_and_active_regions(
            metadata.dump(), ledger, destination.path));
        EXPECT_EQ(ledger.tick(), 1u);
        EXPECT_EQ(ledger.header_tick(), 2u);
    }
}

TEST(WorldSaveService, WaterCursorValidationPrecedesMetadataAndLedgerReplacement) {
    using Luminumbra::world::ActiveRegionLedger;
    using Luminumbra::world::WorldClock;
    TempSaveDir destination("water_cursor_metadata");
    nlohmann::json metadata = {{"container_version", 2}, {"waterSimCursor", 7u}};
    WorldClock(2).write_metadata(metadata);
    ActiveRegionLedger ledger({}, WorldClock(2));
    ledger.set_local_anchor({8, 0, 8});
    ASSERT_TRUE(WorldSaveService::save_metadata_and_active_regions(
        metadata.dump(), ledger, destination.path));
    const auto read = [](const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        std::ostringstream bytes;
        bytes << file.rdbuf();
        return bytes.str();
    };
    const auto metadata_path = destination.path / "world_info.json";
    const auto ledger_path = WorldSaveService::active_regions_path(destination.path);
    const auto metadata_bytes = read(metadata_path);
    const auto ledger_bytes = read(ledger_path);
    WorldClock saved_clock;
    bool saved_required = false;
    std::size_t saved_cursor = 0;
    ASSERT_TRUE(WorldSaveService::read_clock_metadata(
        destination.path, saved_clock, saved_required, nullptr, nullptr, &saved_cursor));
    EXPECT_EQ(saved_cursor, 7u);
    ActiveRegionLedger rewound({}, WorldClock(1));
    rewound.set_local_anchor({8, 0, 8});
    const auto rewound_bytes = rewound.encode();
    WorldClock(1).write_metadata(metadata);
    for (const auto& invalid : {nlohmann::json(nullptr),
                                nlohmann::json(-1),
                                nlohmann::json(1.5),
                                nlohmann::json(true),
                                nlohmann::json("unknown"),
                                nlohmann::json::object(),
                                nlohmann::json::array()}) {
        SCOPED_TRACE(invalid.dump());
        metadata["waterSimCursor"] = invalid;
        EXPECT_FALSE(WorldSaveService::save_metadata(metadata.dump(), destination.path));
        EXPECT_EQ(read(metadata_path), metadata_bytes);
        ASSERT_TRUE(WorldSaveService::write_file_durably(metadata_path, metadata_bytes));
        // A rewind writes the ledger first, so validation must precede both writers.
        auto candidate = rewound;
        EXPECT_FALSE(WorldSaveService::save_metadata_and_active_regions(
            metadata.dump(), candidate, destination.path));
        EXPECT_EQ(read(metadata_path), metadata_bytes);
        EXPECT_EQ(read(ledger_path), ledger_bytes);
        EXPECT_EQ(candidate.encode(), rewound_bytes);
        ASSERT_TRUE(WorldSaveService::write_file_durably(ledger_path, ledger_bytes));
        auto damaged = metadata;
        WorldClock(2).write_metadata(damaged);
        ASSERT_TRUE(WorldSaveService::write_file_durably(metadata_path, damaged.dump()));
        WorldClock clock;
        bool required = false;
        EXPECT_FALSE(WorldSaveService::read_clock_metadata(destination.path, clock, required));
        EXPECT_FALSE(WorldSaveService::validate_save(destination.path));
        ASSERT_TRUE(WorldSaveService::write_file_durably(metadata_path, metadata_bytes));
    }
    for (const std::string& partial : {std::string("{"), std::string("[]")}) {
        EXPECT_FALSE(
            WorldSaveService::save_metadata_and_active_regions(partial, rewound, destination.path));
        EXPECT_EQ(read(metadata_path), metadata_bytes);
        EXPECT_EQ(read(ledger_path), ledger_bytes);
    }
    metadata.erase("waterSimCursor");
    ASSERT_TRUE(WorldSaveService::save_metadata_and_active_regions(
        metadata.dump(), rewound, destination.path));
    WorldClock clock;
    bool required = false;
    std::size_t cursor = 9;
    ASSERT_TRUE(WorldSaveService::read_clock_metadata(
        destination.path, clock, required, nullptr, nullptr, &cursor));
    EXPECT_EQ(cursor, 0u);
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

TEST(WorldSaveService, SaveBatchRefusesCorruptInputsAndPoisonsFailedPayloads) {
    const TempSaveDir root("save-batch");
    EXPECT_FALSE(WorldSaveService::SaveBatch::begin({}));
    auto batch = WorldSaveService::SaveBatch::begin(root.path);
    if (!batch) {
        ADD_FAILURE() << "Could not begin a fresh save operation";
        return;
    }
    EXPECT_FALSE(batch->has_world_save());
    EXPECT_FALSE(batch->has_chunk_snapshot());
    WorldStreamingState state;
    auto chunk = state.get_or_create_chunk({0, 0, 0});
    chunk->heightmap_data = {12.0f};
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(batch->save_world(state, nullptr, true));
    auto existing = WorldSaveService::SaveBatch::begin(root.path);
    if (!existing) {
        ADD_FAILURE() << "Could not begin an existing save operation";
        return;
    }
    EXPECT_TRUE(existing->has_chunk_snapshot());
    EXPECT_TRUE(existing->has_world_save());
    WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    EXPECT_FALSE(existing->save_world(state, nullptr, true));
    EXPECT_FALSE(existing->save_metadata("{}"));
    EXPECT_FALSE(std::filesystem::exists(root.path / "world_info.json"));
    // A fresh operation validates the installed image and can write metadata.
    auto retry = WorldSaveService::SaveBatch::begin(root.path);
    if (!retry) {
        ADD_FAILURE() << "Could not retry the save operation";
        return;
    }
    EXPECT_TRUE(retry->save_metadata(R"({"container_version":2})"));
    const auto region = WorldSaveService::region_file_path(root.path, 0, 0);
    std::ofstream(region, std::ios::binary | std::ios::trunc) << "partial";
    std::vector<std::string> errors;
    EXPECT_FALSE(WorldSaveService::SaveBatch::begin(root.path, &errors));
    ASSERT_FALSE(errors.empty());
    EXPECT_NE(errors.front().find("corrupt region file length"), std::string::npos);
}

TEST(WorldSaveService, SaveBatchEmptyDirtySetAndMoveKeepMetadataOrdering) {
    const TempSaveDir root("save-batch-empty");
    auto batch = WorldSaveService::SaveBatch::begin(root.path);
    if (!batch) {
        ADD_FAILURE() << "Could not begin an empty save operation";
        return;
    }
    WorldStreamingState state;
    EXPECT_FALSE(batch->save_dirty_chunks(state, nullptr, true).saved);
    auto moved = std::move(*batch);
    EXPECT_FALSE(batch->save_metadata("{}"));
    EXPECT_TRUE(moved.save_metadata("{}"));
    EXPECT_TRUE(std::filesystem::exists(root.path / "world_info.json"));
}

TEST(WorldSaveService, AuthoritySavesRefuseRegenerableReplacementOfDurableOwners) {
    for (const bool incremental : {false, true}) {
        SCOPED_TRACE(incremental);
        const TempSaveDir root("authority-replacement");
        WorldStreamingState state;
        auto owner = state.get_or_create_chunk({0, 0, 0});
        owner->heightmap_data = {12.0f};
        owner->mark_voxel_data_dirty();
        WorldSaveService service;
        ASSERT_TRUE(service.save_world(state, root.path, nullptr, true));
        const auto path = WorldSaveService::region_file_path(root.path, 0, 0);
        std::vector<WorldSaveService::ContainerRecord> before;
        ASSERT_TRUE(WorldSaveService::read_container_records(path, before));
        state.clear();
        const auto generated = state.get_or_create_chunk({0, 0, 0});
        generated->heightmap_data = {3.0f};
        auto dirty = state.get_or_create_chunk({1, 0, 0});
        dirty->heightmap_data = {4.0f};
        dirty->mark_voxel_data_dirty();
        EXPECT_FALSE(WorldSaveService::persist_chunk_authority(root.path, nullptr));
        EXPECT_FALSE(WorldSaveService::persist_chunk_authority(root.path, generated));
        EXPECT_FALSE(WorldSaveService::persist_chunk_authority({}, dirty));
        if (incremental)
            EXPECT_FALSE(service.save_dirty_chunks(state, root.path, nullptr, true).saved);
        else
            EXPECT_FALSE(service.save_world(state, root.path, nullptr, true));
        EXPECT_TRUE(dirty->is_voxel_data_dirty());
        std::vector<WorldSaveService::ContainerRecord> after;
        ASSERT_TRUE(WorldSaveService::read_container_records(path, after));
        ASSERT_EQ(after.size(), before.size());
        ASSERT_EQ(after.size(), 1u);
        EXPECT_EQ(after.front().payload, before.front().payload);
        EXPECT_EQ(after.front().flags, before.front().flags);
        EXPECT_TRUE(WorldSaveService::validate_save(root.path));
    }
}

TEST(WorldSaveService, ExplicitOwnershipRejectsContradictoryContainerFlags) {
    for (const bool owned : {false, true}) {
        SCOPED_TRACE(owned);
        const TempSaveDir root("authority-header");
        WorldStreamingState state;
        const auto chunk = state.get_or_create_chunk({0, 0, 0});
        chunk->heightmap_data = {12.0f};
        if (owned)
            chunk->mark_voxel_data_dirty();
        const WorldSaveService::ContainerRecord record{
            chunk->get_id(),
            0,
            static_cast<std::uint8_t>(owned ? 0 : 1),
            Luminumbra::Persistence::SerializeWorldStreamingStateSnapshotJson(state, true)};
        const auto path = WorldSaveService::region_file_path(root.path, 0, 0);
        ASSERT_TRUE(WorldSaveService::upsert_container_records(path, {record}));
        WorldStreamingState loaded;
        std::vector<std::string> errors;
        EXPECT_FALSE(WorldSaveService{}.load_world(loaded, root.path, errors));
        EXPECT_TRUE(loaded.empty());
        EXPECT_FALSE(errors.empty());
        EXPECT_FALSE(WorldSaveService::SaveBatch::begin(root.path));
        EXPECT_FALSE(WorldSaveService::validate_save(root.path));
    }
}
