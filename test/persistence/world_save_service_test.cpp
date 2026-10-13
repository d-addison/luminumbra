#include "gtest/gtest.h"

#include "fields/FieldPageState.h"
#include "persistence/SaveTransaction.h"
#include "persistence/WorldPersistenceRoundtrip.h"
#include "persistence/WorldSaveService.h"
#include "systems/SHIELD_WorldSystem.h"
#include "world/GroundObjectStore.h"
#include "world/MarchingCubes.h"
#include "world/WorldStreamingState.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
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
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("luminumbra_world_save_service_" + tag + "_" + std::to_string(stamp));
    std::filesystem::create_directories(dir);
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
    EXPECT_FALSE(std::filesystem::exists(save_dir.path / "chunks"));
    nlohmann::json metadata = {{"container_version", 2}};
    Luminumbra::world::WorldClock(123, {12000, 4}).write_metadata(metadata);
    const auto valid = metadata.dump(4) + "\n";
    ASSERT_TRUE(WorldSaveService::save_metadata(valid, save_dir.path));
    EXPECT_EQ(read(), valid);
    const auto index_path = Luminumbra::Persistence::snapshot_index_path(save_dir.path);
    ASSERT_TRUE(std::filesystem::exists(index_path));
    std::ifstream index_file(index_path, std::ios::binary);
    const std::string index_before(std::istreambuf_iterator<char>{index_file}, {});
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
    // Clock-bearing metadata is an enabled indexed snapshot, so its region directory
    // already exists. Refusal must preserve the published index, not remove that directory.
    std::ifstream index_after_file(index_path, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>{index_after_file}, {}), index_before);
    EXPECT_EQ(read(), damaged);
    std::ofstream(metadata_path, std::ios::binary | std::ios::trunc) << "{";
    EXPECT_NO_THROW(EXPECT_FALSE(WorldSaveService::save_metadata(valid, save_dir.path, &errors)));
    EXPECT_EQ(read(), "{");
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

// ---- Ground objects (S5) persistence ----

namespace {

Luminumbra::world::GroundObject
MakeGroundObject(std::uint64_t id, std::int64_t x_mm, std::uint64_t spawn_tick) {
    Luminumbra::world::GroundObject object;
    object.id = id;
    object.type = 3;
    object.x_mm = x_mm;
    object.y_mm = 1000;
    object.z_mm = -2000;
    object.spawn_tick = spawn_tick;
    object.payload = 7;
    return object;
}

std::string JoinErrors(const std::vector<std::string>& errors) {
    std::string joined;
    for (const std::string& message : errors)
        joined += message + "\n";
    return joined;
}

} // namespace

TEST(WorldSaveService, GroundObjectsSaveLoadRoundTripAndEmptyStoreRemovesFile) {
    namespace fs = std::filesystem;
    using Luminumbra::world::GroundObjectStore;
    TempSaveDir save_dir("ground_roundtrip");

    GroundObjectStore store;
    ASSERT_TRUE(store.insert(MakeGroundObject(1, 100, 0)));
    ASSERT_TRUE(store.insert(MakeGroundObject(5, -600000, 2)));
    std::vector<std::string> errors;
    const Luminumbra::world::WorldClock clock(2);
    Luminumbra::world::ActiveRegionLedger ledger({}, clock);
    std::uint64_t allocated = 0;
    for (int id = 0; id < 5; ++id)
        ASSERT_TRUE(ledger.allocate_persistent_id(allocated));
    for (const auto& [id, object] : store.objects()) {
        (void)id;
        ledger.mark_edited(GroundObjectStore::region_of(object), clock);
    }
    nlohmann::json metadata = {{"container_version", 2}};
    clock.write_metadata(metadata);
    {
        WorldSaveService::SaveFactsScope facts(save_dir.path);
        Luminumbra::Persistence::SaveTransaction transaction(
            save_dir.path, clock.tick(), {}, WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(&errors)) << JoinErrors(errors);
        ASSERT_TRUE(WorldSaveService::save_metadata_and_active_regions(
            metadata.dump(), ledger, save_dir.path, &errors))
            << JoinErrors(errors);
        ASSERT_TRUE(WorldSaveService::save_ground_objects(store, save_dir.path, &errors))
            << JoinErrors(errors);
        ASSERT_TRUE(transaction.commit(&errors)) << JoinErrors(errors);
    }
    EXPECT_TRUE(fs::exists(WorldSaveService::ground_objects_path(save_dir.path)));

    GroundObjectStore loaded;
    ASSERT_TRUE(WorldSaveService::load_ground_objects(loaded, save_dir.path, 100, &errors))
        << JoinErrors(errors);
    EXPECT_EQ(loaded.objects(), store.objects());

    // A stale file without a ledger is an orphan that validate_save refuses to write over, so the
    // empty-store lane is exercised on a directory without the file.
    ASSERT_TRUE(fs::remove(WorldSaveService::ground_objects_path(save_dir.path)));
    ASSERT_TRUE(WorldSaveService::save_ground_objects(GroundObjectStore{}, save_dir.path, &errors))
        << JoinErrors(errors);
    EXPECT_FALSE(fs::exists(WorldSaveService::ground_objects_path(save_dir.path)));
    GroundObjectStore absent;
    ASSERT_TRUE(WorldSaveService::load_ground_objects(absent, save_dir.path, 100, &errors));
    EXPECT_TRUE(absent.empty());
}

TEST(WorldSaveService, GroundObjectIdAtOrAboveNextIdIsRefused) {
    using Luminumbra::world::GroundObjectStore;
    TempSaveDir save_dir("ground_next_id");
    GroundObjectStore store;
    ASSERT_TRUE(store.insert(MakeGroundObject(5, 100, 0)));
    std::vector<std::string> errors;
    ASSERT_TRUE(WorldSaveService::save_ground_objects(store, save_dir.path, &errors));

    GroundObjectStore loaded;
    EXPECT_FALSE(WorldSaveService::load_ground_objects(loaded, save_dir.path, 5, &errors));
    EXPECT_TRUE(loaded.empty()) << "a refused load leaves the output untouched";
    EXPECT_TRUE(WorldSaveService::load_ground_objects(loaded, save_dir.path, 6, &errors));
    EXPECT_EQ(loaded.size(), 1u);
}

TEST(WorldSaveService, CorruptGroundObjectsFileIsRefusedByLoadAndMemberDigest) {
    TempSaveDir save_dir("ground_corrupt");
    const auto path = WorldSaveService::ground_objects_path(save_dir.path);
    std::filesystem::create_directories(path.parent_path());
    {
        std::ofstream out(path, std::ios::binary);
        out << "{\"schema\":\"luminumbra.ground_objects.v1\",\"objects\":[";
    }
    Luminumbra::world::GroundObjectStore loaded;
    std::vector<std::string> errors;
    EXPECT_FALSE(WorldSaveService::load_ground_objects(loaded, save_dir.path, 100, &errors));
    EXPECT_FALSE(errors.empty());

    Luminumbra::Persistence::MemberDigest digest;
    std::string error;
    EXPECT_FALSE(WorldSaveService::member_digest(
        save_dir.path, "chunks/region/ground-objects.json", digest, error));
    EXPECT_FALSE(error.empty());
}

TEST(WorldSaveService, GroundObjectWithoutLedgerIsRefusedByLoadWorld) {
    TempSaveDir save_dir("ground_orphan");
    WorldSaveService service;
    WorldStreamingState original;
    PopulateFixtureWorld(original);
    std::vector<std::string> save_errors;
    ASSERT_TRUE(service.save_world(original, save_dir.path, &save_errors));

    Luminumbra::world::GroundObjectStore store;
    ASSERT_TRUE(store.insert(MakeGroundObject(1, 100, 0)));
    ASSERT_TRUE(WorldSaveService::save_ground_objects(store, save_dir.path, &save_errors));

    WorldStreamingState restored;
    std::vector<std::string> load_errors;
    EXPECT_FALSE(service.load_world(restored, save_dir.path, load_errors));
    EXPECT_NE(JoinErrors(load_errors).find("corrupt ground objects: orphaned object"),
              std::string::npos)
        << JoinErrors(load_errors);
}

TEST(WorldSaveService, RegionDigestIgnoresEmptyGroundBytesAndSeparatesNonEmptyBytes) {
    using Luminumbra::world::RegionKey;
    const std::filesystem::path no_dir;
    const RegionKey key{0, 0};

    const std::uint64_t plain = WorldSaveService::region_simulation_digest(no_dir, key, {});
    WorldStreamingState empty_state;
    EXPECT_EQ(
        plain,
        std::stoull(Luminumbra::Persistence::ComputeRegionContentHash(empty_state), nullptr, 16));

    Luminumbra::Persistence::RegionDigestInputs first;
    first.ground_object_bytes = "1,3,100,1000,-2000,0,0,0,7,0;";
    Luminumbra::Persistence::RegionDigestInputs second;
    second.ground_object_bytes = "1,3,101,1000,-2000,0,0,0,7,0;";
    const std::uint64_t with_first = WorldSaveService::region_simulation_digest(no_dir, key, first);
    const std::uint64_t with_second =
        WorldSaveService::region_simulation_digest(no_dir, key, second);
    EXPECT_NE(with_first, plain);
    EXPECT_NE(with_second, plain);
    EXPECT_NE(with_first, with_second);
}

// ---- Field pages (S6) persistence ----

namespace {

void TouchFile(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
}

// Regions (0,0) and (1,0) both own cells of page x=1: cells 16..20 belong to region 0 and cells
// 21..31 to region 1.
Luminumbra::world::FieldPageState MakeFieldState() {
    Luminumbra::world::FieldPageState state;
    state.activate_region({0, 0}, 42, 16);
    state.activate_region({1, 0}, 42, 16);
    return state;
}

} // namespace

TEST(WorldSaveService, FieldPagesSaveLoadRoundTripAndEmptyStateRemovesFile) {
    namespace fs = std::filesystem;
    using Luminumbra::world::FieldPageState;
    TempSaveDir save_dir("field_pages_roundtrip");

    const FieldPageState state = MakeFieldState();
    ASSERT_TRUE(state.owns_region({0, 0}));
    std::vector<std::string> errors;
    const Luminumbra::world::WorldClock clock(16);
    Luminumbra::world::ActiveRegionLedger ledger({}, clock);
    ledger.mark_edited({0, 0}, clock);
    ledger.mark_edited({1, 0}, clock);
    nlohmann::json metadata = {{"container_version", 2}};
    clock.write_metadata(metadata);
    {
        WorldSaveService::SaveFactsScope facts(save_dir.path);
        Luminumbra::Persistence::SaveTransaction transaction(
            save_dir.path, clock.tick(), {}, WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(&errors)) << JoinErrors(errors);
        ASSERT_TRUE(WorldSaveService::save_metadata_and_active_regions(
            metadata.dump(), ledger, save_dir.path, &errors))
            << JoinErrors(errors);
        ASSERT_TRUE(WorldSaveService::save_field_pages(state, save_dir.path, &errors))
            << JoinErrors(errors);
        ASSERT_TRUE(transaction.commit(&errors)) << JoinErrors(errors);
    }
    ASSERT_TRUE(WorldSaveService::validate_save(save_dir.path, &errors)) << JoinErrors(errors);
    EXPECT_TRUE(fs::exists(WorldSaveService::field_pages_path(save_dir.path)));

    FieldPageState loaded;
    ASSERT_TRUE(WorldSaveService::load_field_pages(loaded, save_dir.path, &errors))
        << JoinErrors(errors);
    EXPECT_EQ(loaded.page_count(), state.page_count());
    EXPECT_EQ(loaded.serialize(), state.serialize());

    // The writer must retire the existing indexed member, preserving a valid ledger/clock pair.
    ASSERT_TRUE(fs::exists(WorldSaveService::field_pages_path(save_dir.path)));
    ASSERT_TRUE(WorldSaveService::save_field_pages(FieldPageState{}, save_dir.path, &errors))
        << JoinErrors(errors);
    ASSERT_TRUE(WorldSaveService::validate_save(save_dir.path, &errors)) << JoinErrors(errors);
    Luminumbra::Persistence::SnapshotIndex index;
    ASSERT_TRUE(Luminumbra::Persistence::read_snapshot_index(save_dir.path, index, &errors));
    const auto member = std::find_if(index.members.begin(), index.members.end(), [](const auto& m) {
        return m.path == FieldPageState::kRelativePath;
    });
    ASSERT_NE(member, index.members.end());
    EXPECT_FALSE(member->present);
    EXPECT_FALSE(fs::exists(WorldSaveService::field_pages_path(save_dir.path)));
    FieldPageState absent = MakeFieldState();
    ASSERT_TRUE(WorldSaveService::load_field_pages(absent, save_dir.path, &errors));
    EXPECT_TRUE(absent.empty());
}

TEST(WorldSaveService, CorruptFieldPagesFileIsRefusedByLoadAndMemberDigest) {
    namespace fs = std::filesystem;
    using Luminumbra::world::FieldPageState;
    TempSaveDir save_dir("field_pages_corrupt");
    const fs::path path = WorldSaveService::field_pages_path(save_dir.path);
    fs::create_directories(path.parent_path());
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "FPG1";
    }

    FieldPageState loaded = MakeFieldState();
    const std::size_t before = loaded.page_count();
    std::vector<std::string> errors;
    EXPECT_FALSE(WorldSaveService::load_field_pages(loaded, save_dir.path, &errors));
    EXPECT_NE(JoinErrors(errors).find("Field pages load failed: "), std::string::npos)
        << JoinErrors(errors);
    EXPECT_EQ(loaded.page_count(), before) << "a refused load leaves the output untouched";

    Luminumbra::Persistence::MemberDigest digest;
    std::string error;
    EXPECT_FALSE(WorldSaveService::member_digest(
        save_dir.path, "chunks/region/field-pages.fpg", digest, error));
    EXPECT_FALSE(error.empty());
}

TEST(WorldSaveService, FieldPagesWithoutLedgerAreRefusedByLoadWorld) {
    TempSaveDir save_dir("field_pages_orphan");
    WorldSaveService service;
    WorldStreamingState original;
    PopulateFixtureWorld(original);
    std::vector<std::string> save_errors;
    ASSERT_TRUE(service.save_world(original, save_dir.path, &save_errors));

    ASSERT_TRUE(WorldSaveService::save_field_pages(MakeFieldState(), save_dir.path, &save_errors))
        << JoinErrors(save_errors);

    WorldStreamingState restored;
    std::vector<std::string> load_errors;
    EXPECT_FALSE(service.load_world(restored, save_dir.path, load_errors));
    EXPECT_NE(JoinErrors(load_errors).find("corrupt field pages: orphaned page"), std::string::npos)
        << JoinErrors(load_errors);
}

TEST(WorldSaveService, FieldPageSpanningTwoRegionsNeedsEveryOwnerInTheLedger) {
    using Luminumbra::world::ActiveRegionLedger;
    using Luminumbra::world::RegionKey;
    using Luminumbra::world::WorldClock;
    TempSaveDir save_dir("field_pages_two_owners");
    std::vector<std::string> errors;

    ActiveRegionLedger ledger;
    EXPECT_TRUE(WorldSaveService::validate_field_pages(save_dir.path, ledger, &errors))
        << "an absent file is valid";

    const Luminumbra::world::FieldPageState state = MakeFieldState();
    ASSERT_TRUE(state.owns_region({0, 0}));
    ASSERT_TRUE(state.owns_region({1, 0}));
    ASSERT_TRUE(WorldSaveService::save_field_pages(state, save_dir.path, &errors))
        << JoinErrors(errors);
    // validate_field_pages only checks that the ledger file exists; it never decodes it here.
    TouchFile(WorldSaveService::active_regions_path(save_dir.path));

    ledger.mark_edited(RegionKey{0, 0}, WorldClock{});
    EXPECT_FALSE(WorldSaveService::validate_field_pages(save_dir.path, ledger, &errors));
    EXPECT_NE(JoinErrors(errors).find("corrupt field pages: orphaned page"), std::string::npos)
        << JoinErrors(errors);

    ledger.mark_edited(RegionKey{1, 0}, WorldClock{});
    errors.clear();
    EXPECT_TRUE(WorldSaveService::validate_field_pages(save_dir.path, ledger, &errors))
        << JoinErrors(errors);
}

TEST(WorldSaveService, FieldPageDigestCompositionTable) {
    using Luminumbra::Persistence::RegionDigestInputs;
    using Luminumbra::Persistence::StableChecksum;
    using Luminumbra::world::RegionKey;
    const std::filesystem::path no_dir;
    const RegionKey key{0, 0};
    const std::string ground = "1,3,100,1000,-2000,0,0,0,7,0;";
    const std::string pages = MakeFieldState().region_bytes(key);
    ASSERT_FALSE(pages.empty());

    WorldStreamingState empty_state;
    const std::string content = Luminumbra::Persistence::ComputeRegionContentHash(empty_state);
    const auto digest = [&](const std::string& ground_bytes, const std::string& page_bytes) {
        RegionDigestInputs inputs;
        inputs.ground_object_bytes = ground_bytes;
        inputs.field_page_bytes = page_bytes;
        return WorldSaveService::region_simulation_digest(no_dir, key, inputs);
    };

    const std::uint64_t neither = digest("", "");
    const std::uint64_t ground_only = digest(ground, "");
    const std::uint64_t pages_only = digest("", pages);
    const std::uint64_t both = digest(ground, pages);

    // Neither is the content hash; ground only is the S5 composition of the same inputs.
    EXPECT_EQ(neither, std::stoull(content, nullptr, 16));
    EXPECT_EQ(ground_only, std::stoull(StableChecksum(content + "|ground:" + ground), nullptr, 16));
    EXPECT_EQ(pages_only, std::stoull(StableChecksum(content + "|pages:" + pages), nullptr, 16));
    EXPECT_EQ(both,
              std::stoull(
                  StableChecksum(StableChecksum(content + "|ground:" + ground) + "|pages:" + pages),
                  nullptr,
                  16));
    EXPECT_NE(neither, ground_only);
    EXPECT_NE(neither, pages_only);
    EXPECT_NE(ground_only, pages_only);
    EXPECT_NE(both, ground_only);
    EXPECT_NE(both, pages_only);
}

TEST(WorldSaveService, IndexedAbsentEnergyRefusesResealedAgeBeyondSavedClock) {
    using namespace Luminumbra::world;
    namespace P = Luminumbra::Persistence;
    TempSaveDir save_dir("indexed_absent_energy_future_age");
    const WorldClock clock(16);
    ActiveRegionLedger ledger({}, clock);
    ledger.mark_edited({0, 0}, clock);
    ASSERT_TRUE(ledger.set_energy_age({0, 0}, 16));
    nlohmann::json metadata = {{"container_version", 2}};
    clock.write_metadata(metadata);
    std::vector<std::string> errors;
    {
        WorldSaveService::SaveFactsScope facts(save_dir.path);
        P::SaveTransaction transaction(
            save_dir.path, clock.tick(), {}, WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(&errors)) << JoinErrors(errors);
        ASSERT_TRUE(WorldSaveService::save_metadata_and_active_regions(
            metadata.dump(), ledger, save_dir.path, &errors))
            << JoinErrors(errors);
        ASSERT_TRUE(transaction.commit(&errors)) << JoinErrors(errors);
    }
    WorldSaveService service;
    WorldStreamingState control;
    ASSERT_TRUE(service.load_world(control, save_dir.path, errors)) << JoinErrors(errors);
    P::SnapshotIndex index;
    ASSERT_TRUE(P::read_snapshot_index(save_dir.path, index, &errors));
    const auto energy =
        std::find_if(index.members.begin(), index.members.end(), [](const auto& member) {
            return member.path == "aether_state.efs";
        });
    ASSERT_NE(energy, index.members.end());
    ASSERT_FALSE(energy->present);
    ASSERT_FALSE(std::filesystem::exists(save_dir.path / "aether_state.efs"));
    // Keep owner, ARL header and saved clock valid; only the semantic ARL4 age is future.
    ASSERT_TRUE(ledger.set_energy_age({0, 0}, 17));
    const auto corrupt = ledger.encode(); // codec reseals the exact ARL bytes
    ActiveRegionLedger decoded;
    std::string error;
    ASSERT_TRUE(ActiveRegionLedger::decode(corrupt, decoded, error)) << error;
    ASSERT_EQ(decoded.header_tick(), clock.tick());
    const auto arl =
        std::find_if(index.members.begin(), index.members.end(), [](const auto& member) {
            return member.path == "chunks/region/active-regions.arl";
        });
    ASSERT_NE(arl, index.members.end());
    arl->bytes = corrupt.size();
    arl->fnv64 = P::fnv1a64_bytes(corrupt.data(), corrupt.size());
    std::ofstream(WorldSaveService::active_regions_path(save_dir.path),
                  std::ios::binary | std::ios::trunc)
        << corrupt;
    std::ofstream(P::snapshot_index_path(save_dir.path), std::ios::binary | std::ios::trunc)
        << P::encode_snapshot_index(index);
    WorldStreamingState refused;
    AddFixtureChunk(refused, {0, 0, 0}, ChunkState::Idle, 1);
    errors.clear();
    EXPECT_FALSE(service.load_world(refused, save_dir.path, errors));
    EXPECT_NE(JoinErrors(errors).find("corrupt regional energy field age"), std::string::npos)
        << JoinErrors(errors);
    EXPECT_TRUE(refused.snapshot_chunks().empty());
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
    }
    const auto empty_digest = WorldSaveService::region_simulation_digest({}, {0, 0}, inputs);
    for (const RegionKey key : {RegionKey{-32768, -32768},
                                RegionKey{-32768, 32767},
                                RegionKey{32767, -32768},
                                RegionKey{32767, 32767},
                                RegionKey{0, 0}}) {
        EXPECT_EQ(WorldSaveService::region_simulation_digest({}, key, inputs), empty_digest);
        EXPECT_EQ(WorldSaveService::region_simulation_digest(missing_save, key, inputs),
                  empty_digest);
        WorldStreamingState region;
        const auto chunk =
            AddFixtureChunk(region, IVec3(key.x * 32, 0, key.z * 32), ChunkState::Idle, 1u);
        chunk->mark_voxel_data_dirty();
        const Luminumbra::Persistence::RegionDigestInputs edited_inputs{{chunk}, {}, {}, {}, {}};
        const auto expected_digest =
            std::stoull(Luminumbra::Persistence::ComputeRegionContentHash(region), nullptr, 16);
        EXPECT_EQ(WorldSaveService::region_simulation_digest({}, key, edited_inputs),
                  expected_digest);
        EXPECT_EQ(WorldSaveService::region_simulation_digest(missing_save, key, edited_inputs),
                  expected_digest);
    }
    EXPECT_FALSE(std::filesystem::exists(missing_save));
}
