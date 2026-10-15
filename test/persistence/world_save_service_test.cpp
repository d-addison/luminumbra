#include "gtest/gtest.h"

#include "../support/UniqueTempPath.h"

#include "fields/FieldPageState.h"
#include "persistence/ChunkValidationCache.h"
#include "persistence/SaveTransaction.h"
#include "persistence/WorldPersistenceRoundtrip.h"
#include "persistence/WorldSaveService.h"
#include "systems/SHIELD_WorldSystem.h"
#include "world/GroundObjectStore.h"
#include "world/MarchingCubes.h"
#include "world/WorldStreamingState.h"

#include <algorithm>
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
#include <thread>
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

    // Keep the valid indexed member present so only the empty-store writer can remove it.
    {
        WorldSaveService::SaveFactsScope facts(save_dir.path);
        ASSERT_TRUE(facts.validate(&errors)) << JoinErrors(errors);
        Luminumbra::Persistence::SaveTransaction transaction(
            save_dir.path, clock.tick(), {}, WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(&errors)) << JoinErrors(errors);
        ASSERT_TRUE(transaction.protect({"chunks/region/ground-objects.json"}, &errors));
        ASSERT_TRUE(fs::exists(WorldSaveService::ground_objects_path(save_dir.path)));
        ASSERT_TRUE(
            WorldSaveService::save_ground_objects(GroundObjectStore{}, save_dir.path, &errors))
            << JoinErrors(errors);
        ASSERT_TRUE(transaction.commit(&errors)) << JoinErrors(errors);
    }
    EXPECT_FALSE(fs::exists(WorldSaveService::ground_objects_path(save_dir.path)));
    ASSERT_TRUE(WorldSaveService::validate_save(save_dir.path, &errors)) << JoinErrors(errors);
    Luminumbra::Persistence::SnapshotIndex index;
    ASSERT_TRUE(Luminumbra::Persistence::read_snapshot_index(save_dir.path, index, &errors));
    const auto member = std::find_if(index.members.begin(), index.members.end(), [](const auto& m) {
        return m.path == "chunks/region/ground-objects.json";
    });
    ASSERT_NE(member, index.members.end());
    EXPECT_FALSE(member->present);
    GroundObjectStore absent;
    ASSERT_TRUE(WorldSaveService::load_ground_objects(absent, save_dir.path, 100, &errors));
    EXPECT_TRUE(absent.empty());
    WorldStreamingState reopened;
    ASSERT_TRUE(WorldSaveService{}.load_world(reopened, save_dir.path, errors))
        << JoinErrors(errors);
    EXPECT_TRUE(errors.empty()) << JoinErrors(errors);
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
    // Indexing validates full SDF lattices; the tiny legacy chunk fixtures are
    // deliberately incomplete. A manifest-only baseline isolates the orphan
    // refusal without introducing an unrelated invalid chunk member.
    std::vector<std::string> save_errors;
    ASSERT_TRUE(service.save_world(original, save_dir.path, &save_errors))
        << JoinErrors(save_errors);

    Luminumbra::world::GroundObjectStore store;
    ASSERT_TRUE(store.insert(MakeGroundObject(1, 100, 0)));
    // Install a valid indexed pair first; successful writers never publish orphaned objects.
    using namespace Luminumbra::world;
    const WorldClock clock;
    ActiveRegionLedger ledger;
    std::uint64_t id = 0;
    ASSERT_TRUE(ledger.allocate_persistent_id(id));
    ledger.mark_edited({0, 0}, clock);
    nlohmann::json metadata = {{"container_version", 2}};
    clock.write_metadata(metadata);
    ASSERT_TRUE(WorldSaveService::save_metadata_and_active_regions(
        metadata.dump(), ledger, save_dir.path, &save_errors));
    ASSERT_TRUE(WorldSaveService::save_ground_objects(store, save_dir.path, &save_errors))
        << JoinErrors(save_errors);
    // Inject a semantically corrupt but checksum-consistent snapshot for the reader.
    ASSERT_TRUE(std::filesystem::remove(WorldSaveService::active_regions_path(save_dir.path)));
    Luminumbra::Persistence::SnapshotIndex index;
    ASSERT_TRUE(Luminumbra::Persistence::read_snapshot_index(save_dir.path, index, &save_errors));
    for (auto& member : index.members)
        if (member.path == "chunks/region/active-regions.arl") {
            member.present = false;
            member.bytes = 0;
            member.fnv64 = 0;
        }
    ASSERT_TRUE(WorldSaveService::durable_write_file(
        Luminumbra::Persistence::snapshot_index_path(save_dir.path),
        Luminumbra::Persistence::encode_snapshot_index(index),
        &save_errors));

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

    Luminumbra::Persistence::RegionDigestInputs first{
        .live = {}, .parked = {}, .durable = {}, .ground_object_bytes = {}, .field_page_bytes = {}};
    first.ground_object_bytes = "1,3,100,1000,-2000,0,0,0,7,0;";
    Luminumbra::Persistence::RegionDigestInputs second{
        .live = {}, .parked = {}, .durable = {}, .ground_object_bytes = {}, .field_page_bytes = {}};
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
        const RegionDigestInputs inputs{.live = {},
                                        .parked = {},
                                        .durable = {},
                                        .ground_object_bytes = ground_bytes,
                                        .field_page_bytes = page_bytes};
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
        // Never-present members are omitted by the index. Install a valid owned
        // record first, then retire it through the writer to create indexed absence.
        ASSERT_TRUE(WorldSaveService::write_member_bytes(
            save_dir.path, "aether_state.efs", "EFS1 1 0\nP 0 0 0:1\n", &errors))
            << JoinErrors(errors);
        ASSERT_TRUE(transaction.commit(&errors)) << JoinErrors(errors);
    }
    ASSERT_TRUE(WorldSaveService::remove_member(save_dir.path, "aether_state.efs", &errors))
        << JoinErrors(errors);
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
    EXPECT_EQ(
        WorldSaveService::region_simulation_digest(root.path,
                                                   {0, 0},
                                                   RegionDigestInputs{.live = chunks,
                                                                      .parked = {},
                                                                      .durable = {},
                                                                      .ground_object_bytes = {},
                                                                      .field_page_bytes = {}}),
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
    const auto settled = freeze({},
                                RegionDigestInputs{.live = resident.snapshot_chunks(),
                                                   .parked = {},
                                                   .durable = {},
                                                   .ground_object_bytes = {},
                                                   .field_page_bytes = {}});
    ASSERT_NE(settled,
              freeze({},
                     RegionDigestInputs{.live = {},
                                        .parked = {},
                                        .durable = {},
                                        .ground_object_bytes = {},
                                        .field_page_bytes = {}}));
    EXPECT_EQ(freeze(root.path,
                     RegionDigestInputs{.live = resident.snapshot_chunks(),
                                        .parked = {},
                                        .durable = {},
                                        .ground_object_bytes = {},
                                        .field_page_bytes = {}}),
              settled);
    EXPECT_EQ(freeze({},
                     RegionDigestInputs{.live = {},
                                        .parked = resident.snapshot_chunks(),
                                        .durable = {},
                                        .ground_object_bytes = {},
                                        .field_page_bytes = {}}),
              settled);

    // Incomplete live/parked content must fall back to the edited durable Loading record.
    auto incomplete = std::make_shared<Chunk>(chunk->get_coords());
    incomplete->heightmap_data = {99.0f};
    incomplete->mark_voxel_data_dirty();
    incomplete->set_state(ChunkState::Loading);
    EXPECT_EQ(freeze({},
                     RegionDigestInputs{.live = {incomplete},
                                        .parked = {},
                                        .durable = {},
                                        .ground_object_bytes = {},
                                        .field_page_bytes = {}}),
              freeze({},
                     RegionDigestInputs{.live = {},
                                        .parked = {},
                                        .durable = {},
                                        .ground_object_bytes = {},
                                        .field_page_bytes = {}}));
    EXPECT_EQ(freeze({},
                     RegionDigestInputs{.live = {},
                                        .parked = {incomplete},
                                        .durable = {},
                                        .ground_object_bytes = {},
                                        .field_page_bytes = {}}),
              freeze({},
                     RegionDigestInputs{.live = {},
                                        .parked = {},
                                        .durable = {},
                                        .ground_object_bytes = {},
                                        .field_page_bytes = {}}));
    EXPECT_EQ(freeze(root.path,
                     RegionDigestInputs{.live = {incomplete},
                                        .parked = {incomplete},
                                        .durable = {},
                                        .ground_object_bytes = {},
                                        .field_page_bytes = {}}),
              settled);

    resident.clear();
    chunk.reset();
    saved.clear();
    EXPECT_EQ(freeze(root.path,
                     RegionDigestInputs{.live = {},
                                        .parked = {},
                                        .durable = {},
                                        .ground_object_bytes = {},
                                        .field_page_bytes = {}}),
              settled);
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
                {},
                {0, 0},
                RegionDigestInputs{.live = {chunk},
                                   .parked = {},
                                   .durable = {},
                                   .ground_object_bytes = {},
                                   .field_page_bytes = {}});
            if (edited)
                ASSERT_NE(settled, empty);
            else
                ASSERT_EQ(settled, empty);
            chunk->set_state(lifecycle);
            const auto in_memory = lifecycle == ChunkState::Loading ? empty : settled;
            EXPECT_EQ(WorldSaveService::region_simulation_digest(
                          {},
                          {0, 0},
                          RegionDigestInputs{.live = {chunk},
                                             .parked = {},
                                             .durable = {},
                                             .ground_object_bytes = {},
                                             .field_page_bytes = {}}),
                      in_memory);
            EXPECT_EQ(WorldSaveService::region_simulation_digest(
                          {},
                          {0, 0},
                          RegionDigestInputs{.live = {},
                                             .parked = {chunk},
                                             .durable = {},
                                             .ground_object_bytes = {},
                                             .field_page_bytes = {}}),
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
    const Luminumbra::Persistence::RegionDigestInputs inputs{
        .live = {}, .parked = {}, .durable = {}, .ground_object_bytes = {}, .field_page_bytes = {}};
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
        EXPECT_EQ(WorldSaveService::region_simulation_digest({}, key, inputs), empty_digest);
        EXPECT_EQ(WorldSaveService::region_simulation_digest(missing_save, key, inputs),
                  empty_digest);
        WorldStreamingState region;
        const auto chunk =
            AddFixtureChunk(region, IVec3(key.x * 32, 0, key.z * 32), ChunkState::Idle, 1u);
        chunk->mark_voxel_data_dirty();
        const Luminumbra::Persistence::RegionDigestInputs edited_inputs{.live = {chunk},
                                                                        .parked = {},
                                                                        .durable = {},
                                                                        .ground_object_bytes = {},
                                                                        .field_page_bytes = {}};
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

namespace {
namespace CachePersistence = Luminumbra::Persistence;
using ValidationCache = CachePersistence::ChunkValidationCache;

void WriteCacheFixture(const std::filesystem::path& path,
                       bool invalid_water = false,
                       const IVec3& coords = IVec3(0, 0, 0)) {
    WorldStreamingState state;
    auto chunk = state.get_or_create_chunk(coords);
    chunk->set_state(ChunkState::Idle);
    chunk->mark_sdf_loaded_or_edited();
    if (invalid_water) {
        // The permissive decoder accepts legacy mirrors; strict indexed water
        // validation must still reject them after permissive facts have warmed.
        chunk->has_water_sim.store(true);
        chunk->water_level_data = {1.0f};
    }
    std::vector<std::string> errors;
    ASSERT_TRUE(WorldSaveService{}.save_world(state, path, &errors)) << JoinErrors(errors);
    ASSERT_TRUE(errors.empty()) << JoinErrors(errors);
}

void IndexCacheFixture(const std::filesystem::path& path) {
    CachePersistence::SaveTransaction transaction(path, 0, {}, WorldSaveService::member_digest);
    std::vector<std::string> errors;
    ASSERT_TRUE(transaction.begin(&errors)) << JoinErrors(errors);
    ASSERT_TRUE(transaction.protect(
        {"chunks/region/r.0.0.lmr", "chunks/region/world-manifest.json"}, &errors));
    ASSERT_TRUE(transaction.commit(&errors)) << JoinErrors(errors);
}

bool ValidateCacheFixture(const std::filesystem::path& path,
                          std::vector<std::string>& errors,
                          std::vector<IVec3>* inventory = nullptr) {
    WorldStreamingState discarded;
    return WorldSaveService{}.load_world(
        discarded, path, errors, {}, [](const IVec3&) { return false; }, inventory);
}

std::string CacheFileBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream bytes;
    bytes << input.rdbuf();
    return bytes.str();
}
} // namespace

TEST(WorldSaveService, ValidationCacheColdWarmAndMaterializedLoadsAreEquivalent) {
    TempSaveDir save_dir("validation_cache_equivalence");
    ASSERT_NO_FATAL_FAILURE(WriteCacheFixture(save_dir.path));
    ASSERT_NO_FATAL_FAILURE(IndexCacheFixture(save_dir.path));
    ValidationCache cache;
    std::vector<std::string> cold_errors, miss_errors, hit_errors;
    std::vector<IVec3> cold_inventory, miss_inventory, hit_inventory;
    ASSERT_TRUE(ValidateCacheFixture(save_dir.path, cold_errors, &cold_inventory));
    ASSERT_EQ(cold_inventory.size(), 1u);
    EXPECT_EQ(cold_inventory.front(), IVec3(0, 0, 0));
    {
        const ValidationCache::Scope scope(cache);
        ASSERT_TRUE(ValidateCacheFixture(save_dir.path, miss_errors, &miss_inventory));
        EXPECT_EQ(cache.stats().misses, 1u);
        ASSERT_TRUE(ValidateCacheFixture(save_dir.path, hit_errors, &hit_inventory));
        EXPECT_EQ(cache.stats().hits, 1u);
        EXPECT_EQ(cache.stats().misses, 1u);
        WorldStreamingState warm_loaded;
        std::vector<std::string> errors;
        ASSERT_TRUE(WorldSaveService{}.load_world(warm_loaded, save_dir.path, errors));
        WorldStreamingState cold_loaded;
        ValidationCache disabled(0);
        const ValidationCache::Scope cold_scope(disabled);
        ASSERT_TRUE(WorldSaveService{}.load_world(cold_loaded, save_dir.path, errors));
        EXPECT_EQ(WorldSaveService{}.world_hash(warm_loaded),
                  WorldSaveService{}.world_hash(cold_loaded));
        EXPECT_TRUE(errors.empty());
    }
    EXPECT_EQ(miss_errors, cold_errors);
    EXPECT_EQ(hit_errors, cold_errors);
    EXPECT_EQ(miss_inventory, cold_inventory);
    EXPECT_EQ(hit_inventory, cold_inventory);
}

TEST(WorldSaveService, ValidationCacheSeparatesPermissiveCommitAndStrictIndexedDecode) {
    TempSaveDir save_dir("validation_cache_modes");
    ASSERT_NO_FATAL_FAILURE(WriteCacheFixture(save_dir.path, true));
    ValidationCache cache;
    {
        const ValidationCache::Scope scope(cache);
        // Commit uses permissive decode, including on the second exact-byte hit.
        ASSERT_NO_FATAL_FAILURE(IndexCacheFixture(save_dir.path));
        CachePersistence::MemberDigest digest;
        std::string error;
        ASSERT_TRUE(WorldSaveService::member_digest(
            save_dir.path, "chunks/region/r.0.0.lmr", digest, error))
            << error;
        EXPECT_EQ(cache.stats().hits, 2u);
        EXPECT_EQ(cache.stats().misses, 1u);
    }
    std::vector<std::string> cold_errors, warm_errors, retry_errors;
    ASSERT_FALSE(ValidateCacheFixture(save_dir.path, cold_errors));
    ASSERT_FALSE(cold_errors.empty());
    {
        const ValidationCache::Scope scope(cache);
        EXPECT_FALSE(ValidateCacheFixture(save_dir.path, warm_errors));
        EXPECT_FALSE(ValidateCacheFixture(save_dir.path, retry_errors));
    }
    EXPECT_EQ(warm_errors, cold_errors);
    EXPECT_EQ(retry_errors, cold_errors);
    EXPECT_EQ(cache.stats().misses, 3u); // Failed strict results never become hits.
    EXPECT_EQ(cache.stats().hits, 2u);
}

TEST(WorldSaveService, CachedRecordSingleByteCorruptionRetainsColdDiagnostic) {
    TempSaveDir save_dir("validation_cache_corruption");
    ASSERT_NO_FATAL_FAILURE(WriteCacheFixture(save_dir.path));
    ASSERT_NO_FATAL_FAILURE(IndexCacheFixture(save_dir.path));
    ValidationCache cache;
    std::vector<std::string> errors;
    {
        const ValidationCache::Scope scope(cache);
        ASSERT_TRUE(ValidateCacheFixture(save_dir.path, errors));
        ASSERT_TRUE(ValidateCacheFixture(save_dir.path, errors));
    }
    const auto path = WorldSaveService::region_file_path(save_dir.path, 0, 0);
    auto bytes = CacheFileBytes(path);
    ASSERT_GT(bytes.size(), 26u); // One complete record header precedes the compressed bytes.
    bytes.back() = static_cast<char>(static_cast<unsigned char>(bytes.back()) ^ 1u);
    ASSERT_TRUE(WorldSaveService::durable_write_file(path, bytes, &errors));
    std::vector<std::string> cold_errors, warm_errors;
    ASSERT_FALSE(ValidateCacheFixture(save_dir.path, cold_errors));
    {
        const ValidationCache::Scope scope(cache);
        EXPECT_FALSE(ValidateCacheFixture(save_dir.path, warm_errors));
    }
    EXPECT_EQ(warm_errors, cold_errors);
    EXPECT_FALSE(warm_errors.empty());
    EXPECT_EQ(cache.stats().hits, 1u);
    EXPECT_EQ(cache.stats().misses, 2u);
}

TEST(WorldSaveService, ValidationCacheEvictionOnlyCausesRevalidation) {
    TempSaveDir first("validation_cache_evict_first"), second("validation_cache_evict_second");
    ASSERT_NO_FATAL_FAILURE(WriteCacheFixture(first.path));
    ASSERT_NO_FATAL_FAILURE(WriteCacheFixture(second.path, false, {1, 0, 0}));
    // Distinct identities exceed the single-entry limit.
    ValidationCache cache(1);
    const ValidationCache::Scope scope(cache);
    std::vector<std::string> errors;
    ASSERT_TRUE(ValidateCacheFixture(first.path, errors));
    ASSERT_TRUE(ValidateCacheFixture(first.path, errors));
    ASSERT_TRUE(ValidateCacheFixture(second.path, errors));
    ASSERT_TRUE(ValidateCacheFixture(first.path, errors));
    ASSERT_TRUE(ValidateCacheFixture(first.path, errors));
    EXPECT_EQ(cache.stats().misses, 3u);
    EXPECT_EQ(cache.stats().evictions, 2u);
    EXPECT_EQ(cache.stats().hits, 2u);
    EXPECT_EQ(cache.stats().entries, 1u);
    EXPECT_TRUE(errors.empty());
}

TEST(WorldSaveService, ValidationCacheKeysAndBoundsIncludeEveryInputAndMode) {
    ValidationCache cache(1, 64);
    const ValidationCache::Key key{1, 7, 0, 0, false};
    const ValidationCache::Facts facts{{0, 0, 0}, 0, false};
    cache.remember(key, "abcdefg", facts);
    EXPECT_TRUE(cache.find(key, "abcdefg").has_value());
    EXPECT_FALSE(cache.find(key, "abcdef").has_value());
    for (std::size_t i = 0; i < 7; ++i) {
        std::string changed = "abcdefg";
        changed[i] = 'X';
        EXPECT_FALSE(cache.find(key, changed).has_value());
    }
    for (int field = 0; field < 5; ++field) {
        auto changed = key;
        if (field == 0)
            ++changed.id;
        if (field == 1)
            ++changed.raw_size;
        if (field == 2)
            ++changed.lod;
        if (field == 3)
            ++changed.flags;
        if (field == 4)
            changed.strict_water = true;
        EXPECT_FALSE(cache.find(changed, "abcdefg").has_value());
    }
    auto other = key;
    ++other.id;
    cache.remember(other, "second", facts);
    EXPECT_EQ(cache.stats().evictions, 1u);
    EXPECT_EQ(cache.stats().entries, 1u);
    EXPECT_LE(cache.stats().payload_bytes, 64u);
    EXPECT_FALSE(cache.find(key, "abcdefg").has_value());
    cache.remember(key, std::string(65, 'x'), facts);
    EXPECT_EQ(cache.stats().entries, 1u);
    EXPECT_FALSE(cache.find(key, std::string(65, 'x')).has_value());
    ValidationCache byte_limited(4, 12);
    byte_limited.remember(key, "abcdefg", facts);
    byte_limited.remember(other, "abcdefg", facts);
    EXPECT_EQ(byte_limited.stats().evictions, 1u);
    EXPECT_EQ(byte_limited.stats().entries, 1u);
    EXPECT_EQ(byte_limited.stats().payload_bytes, 7u);
    EXPECT_FALSE(byte_limited.find(key, "abcdefg").has_value());
    EXPECT_TRUE(byte_limited.find(other, "abcdefg").has_value());
}

TEST(WorldSaveService, ValidationCacheEnforcesDefaultAndClampedBoundsAndRecordCap) {
    EXPECT_EQ(ValidationCache::kMaxEntries, 16384u);
    EXPECT_EQ(ValidationCache::kMaxPayloadBytes, 64u * 1024u * 1024u);
    EXPECT_EQ(ValidationCache::kMaxRecordBytes, 1024u * 1024u);
    const ValidationCache::Facts facts{};
    const std::string payload(ValidationCache::kMaxRecordBytes, 'x');
    for (const bool oversized_limits : {false, true}) {
        SCOPED_TRACE(oversized_limits);
        const auto owner =
            oversized_limits
                ? std::make_unique<ValidationCache>(ValidationCache::kMaxEntries + 1,
                                                    ValidationCache::kMaxPayloadBytes + 1)
                : std::make_unique<ValidationCache>();
        auto& cache = *owner;
        for (std::uint64_t id = 0; id < 65; ++id) {
            cache.remember({id, 0, 0, 0, false}, payload, facts);
            EXPECT_LE(cache.stats().payload_bytes, ValidationCache::kMaxPayloadBytes);
        }
        EXPECT_EQ(cache.stats().payload_bytes, ValidationCache::kMaxPayloadBytes);
        EXPECT_EQ(cache.stats().entries, 64u);
        EXPECT_EQ(cache.stats().evictions, 1u);
        EXPECT_FALSE(cache.find({0, 0, 0, 0, false}, payload).has_value());
        EXPECT_TRUE(cache.find({64, 0, 0, 0, false}, payload).has_value());
        const std::string oversized(ValidationCache::kMaxRecordBytes + 1, 'x');
        cache.remember({65, 0, 0, 0, false}, oversized, facts);
        EXPECT_FALSE(cache.find({65, 0, 0, 0, false}, oversized).has_value());
        EXPECT_EQ(cache.stats().entries, 64u);
        EXPECT_EQ(cache.stats().payload_bytes, ValidationCache::kMaxPayloadBytes);
        EXPECT_EQ(cache.stats().evictions, 1u);
        // Oversized replacements also preserve previously validated exact bytes.
        cache.remember({64, 0, 0, 0, false}, oversized, facts);
        EXPECT_TRUE(cache.find({64, 0, 0, 0, false}, payload).has_value());
        EXPECT_FALSE(cache.find({64, 0, 0, 0, false}, oversized).has_value());
    }
    ValidationCache entry_limited(ValidationCache::kMaxEntries + 1);
    for (std::uint64_t id = 0; id <= ValidationCache::kMaxEntries; ++id)
        entry_limited.remember({id, 0, 0, 0, false}, "x", facts);
    EXPECT_EQ(entry_limited.stats().entries, ValidationCache::kMaxEntries);
    EXPECT_EQ(entry_limited.stats().payload_bytes, ValidationCache::kMaxEntries);
    EXPECT_EQ(entry_limited.stats().evictions, 1u);
    EXPECT_FALSE(entry_limited.find({0, 0, 0, 0, false}, "x").has_value());
    EXPECT_TRUE(
        entry_limited.find({ValidationCache::kMaxEntries, 0, 0, 0, false}, "x").has_value());
}

TEST(WorldSaveService, ValidationCacheClearReleasesBytesAndResetsStatistics) {
    ValidationCache cache(1);
    const ValidationCache::Key first{1, 7, 0, 0, false}, second{2, 7, 0, 0, false};
    const ValidationCache::Facts facts{{1, 0, 0}, 7, true};
    cache.remember(first, "payload", facts);
    ASSERT_TRUE(cache.find(first, "payload").has_value());
    cache.remember(second, "payload", facts);
    ASSERT_FALSE(cache.find(first, "payload").has_value());
    cache.clear();
    const auto cleared = cache.stats();
    EXPECT_EQ(cleared.entries, 0u);
    EXPECT_EQ(cleared.payload_bytes, 0u);
    EXPECT_EQ(cleared.hits, 0u);
    EXPECT_EQ(cleared.misses, 0u);
    EXPECT_EQ(cleared.evictions, 0u);
    EXPECT_FALSE(cache.find(second, "payload").has_value());
    cache.remember(first, "changed", {{2, 0, 0}, 8, false});
    const auto fresh = cache.find(first, "changed");
    ASSERT_TRUE(fresh.has_value());
    if (!fresh.has_value())
        return;
    EXPECT_EQ(fresh->coords, IVec3(2, 0, 0));
    EXPECT_EQ(fresh->sdf_size, 8u);
    EXPECT_FALSE(fresh->authoritative);
    EXPECT_FALSE(cache.find(first, "payload").has_value());
}

TEST(WorldSaveService, ValidationCacheScopesRestoreAndSharedCacheIsThreadSafe) {
    ValidationCache shared, nested;
    auto* previous = ValidationCache::active();
    {
        const ValidationCache::Scope outer(shared);
        EXPECT_EQ(ValidationCache::active(), &shared);
        {
            const ValidationCache::Scope inner(nested);
            EXPECT_EQ(ValidationCache::active(), &nested);
        }
        EXPECT_EQ(ValidationCache::active(), &shared);
        const auto use_cache = [&shared](std::uint64_t id) {
            const ValidationCache::Scope scope(shared);
            EXPECT_EQ(ValidationCache::active(), &shared);
            const ValidationCache::Key key{id, 7, 0, 0, false};
            const ValidationCache::Facts facts{};
            for (int pass = 0; pass < 32; ++pass) {
                shared.remember(key, "payload", facts);
                EXPECT_TRUE(shared.find(key, "payload").has_value());
            }
        };
        std::jthread first(use_cache, 1u), second(use_cache, 2u);
    } // Threads finish before the enclosing scope is restored.
    EXPECT_EQ(ValidationCache::active(), previous);
    EXPECT_EQ(shared.stats().entries, 2u);
    EXPECT_EQ(shared.stats().hits, 64u);
    EXPECT_EQ(shared.stats().misses, 0u);
}

TEST(WorldSaveService, WarmValidationCacheStillChecksCurrentIndexAndRegionMembership) {
    TempSaveDir save_dir("validation_cache_context");
    ASSERT_NO_FATAL_FAILURE(WriteCacheFixture(save_dir.path));
    ASSERT_NO_FATAL_FAILURE(IndexCacheFixture(save_dir.path));
    ValidationCache cache;
    std::vector<std::string> errors;
    {
        const ValidationCache::Scope scope(cache);
        ASSERT_TRUE(ValidateCacheFixture(save_dir.path, errors));
    }
    const auto index_path = CachePersistence::snapshot_index_path(save_dir.path);
    const auto original_index = CacheFileBytes(index_path);
    CachePersistence::SnapshotIndex index;
    ASSERT_TRUE(CachePersistence::read_snapshot_index(save_dir.path, index, &errors));
    const auto region =
        std::find_if(index.members.begin(), index.members.end(), [](const auto& member) {
            return member.path == "chunks/region/r.0.0.lmr";
        });
    ASSERT_NE(region, index.members.end());
    ++region->bytes;
    ASSERT_TRUE(WorldSaveService::durable_write_file(
        index_path, CachePersistence::encode_snapshot_index(index), &errors));
    std::vector<std::string> cold_errors, warm_errors;
    ASSERT_FALSE(ValidateCacheFixture(save_dir.path, cold_errors));
    {
        const ValidationCache::Scope scope(cache);
        EXPECT_FALSE(ValidateCacheFixture(save_dir.path, warm_errors));
    }
    EXPECT_EQ(warm_errors, cold_errors);
    EXPECT_EQ(cache.stats().hits, 1u);
    ASSERT_TRUE(WorldSaveService::durable_write_file(index_path, original_index, &errors));
    // Exact cached bytes moved into a different region still fail contextual identity.
    std::filesystem::rename(WorldSaveService::region_file_path(save_dir.path, 0, 0),
                            WorldSaveService::region_file_path(save_dir.path, 1, 0));
    cold_errors.clear();
    warm_errors.clear();
    ASSERT_FALSE(ValidateCacheFixture(save_dir.path, cold_errors));
    {
        const ValidationCache::Scope scope(cache);
        EXPECT_FALSE(ValidateCacheFixture(save_dir.path, warm_errors));
    }
    EXPECT_EQ(warm_errors, cold_errors);
    ASSERT_FALSE(warm_errors.empty());
    EXPECT_NE(warm_errors.front().find("corrupt chunk region identity or duplicate id"),
              std::string::npos);
    EXPECT_EQ(cache.stats().hits, 2u);
}
