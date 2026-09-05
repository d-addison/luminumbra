//  persistence v2: LMR1 region container round-trip, obsolete-format refusal
// (world-hash equality is the migration gate),.bak retention, and
// O(edited regions) incremental saves. Container spec:
// design decisions, section 3.
#include "gtest/gtest.h"

#include "persistence/WorldPersistenceRoundtrip.h"
#include "persistence/WorldSaveService.h"
#include "world/WorldStreamingState.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
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
        ("luminumbra_region_format_" + tag + "_" + std::to_string(stamp));
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

std::shared_ptr<Chunk>
AddFixtureChunk(WorldStreamingState& state, const IVec3& coords, Luminumbra::u32 salt) {
    auto chunk = state.get_or_create_chunk(coords);
    chunk->set_state(ChunkState::Ready);
    chunk->sdf_data = {-1.0f - static_cast<float>(salt), -0.5f, 0.75f, 2.0f};
    chunk->heightmap_data = {4.0f + static_cast<float>(salt), 5.25f, 6.5f};
    chunk->mesh_vertices = {{Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), salt + 1u},
                            {Vec3(1.0f, 1.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), salt + 2u},
                            {Vec3(0.0f, 1.0f, 1.0f), Vec3(0.0f, 1.0f, 0.0f), salt + 3u}};
    chunk->mesh_indices = {0u, 1u, 2u};
    chunk->water_level_data = {1.0f, 1.5f};
    chunk->water_flow_data = {Vec2(0.125f, -0.25f)};
    chunk->water_sim_terrain_height = {0.5f, 0.75f};
    chunk->current_lod.store(static_cast<int>(salt % 3u), std::memory_order_release);
    chunk->mesh_version.store(3u + salt, std::memory_order_release);
    chunk->has_water_sim.store(true, std::memory_order_release);
    chunk->max_water_delta_last_tick = 0.03125f * static_cast<float>(salt + 1u);
    return chunk;
}

// Chunks spanning three regions including negative region coordinates:
// (0,0,0)/(1,2,3) -> region (0,0); (40,1,5) -> region (1,0);
// (-3,0,-17) -> region (-1,-1).
void PopulateMultiRegionWorld(WorldStreamingState& state) {
    AddFixtureChunk(state, IVec3(0, 0, 0), 1u);
    AddFixtureChunk(state, IVec3(1, 2, 3), 2u);
    AddFixtureChunk(state, IVec3(40, 1, 5), 3u);
    AddFixtureChunk(state, IVec3(-3, 0, -17), 4u);
}

std::string ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

bool HasRegionTemporaryFile(const std::filesystem::path& region_path) {
    const std::string prefix = region_path.filename().string() + ".tmp.";
    std::error_code error;
    for (const auto& entry :
         std::filesystem::directory_iterator(region_path.parent_path(), error)) {
        if (!error && entry.path().filename().string().rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

void WriteV1SnapshotFile(const WorldStreamingState& state, const std::filesystem::path& save_dir) {
    const std::filesystem::path snapshot_path = WorldSaveService::world_state_path(save_dir);
    std::filesystem::create_directories(snapshot_path.parent_path());
    std::ofstream output(snapshot_path, std::ios::binary | std::ios::trunc);
    output << Luminumbra::Persistence::SerializeWorldStreamingStateSnapshotJson(state);
    ASSERT_TRUE(output.good());
}

struct ParsedRecordHeader {
    std::uint64_t id = 0;
    std::uint8_t lod_level = 0;
    std::uint8_t flags = 0;
    std::uint32_t uncompressed_size = 0;
    std::uint32_t compressed_size = 0;
};

struct ParsedRegionFile {
    std::uint16_t version = 0;
    std::vector<ParsedRecordHeader> records;
};

std::uint64_t ReadLeUint(const unsigned char* bytes, int count) {
    std::uint64_t value = 0;
    for (int i = count - 1; i >= 0; --i) {
        value = (value << 8) | static_cast<std::uint64_t>(bytes[i]);
    }
    return value;
}

// Independent parser for the LMR1 layout so the container bytes themselves
// are under test (not just the writer/reader pair agreeing with itself).
ParsedRegionFile ParseRegionFile(const std::filesystem::path& path) {
    ParsedRegionFile parsed;
    const std::string bytes = ReadFileBytes(path);
    EXPECT_GE(bytes.size(), 8u);
    EXPECT_EQ(bytes.substr(0, 4), "LMR1");
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    parsed.version = static_cast<std::uint16_t>(ReadLeUint(data + 4, 2));
    const auto record_count = static_cast<std::uint16_t>(ReadLeUint(data + 6, 2));
    std::size_t payload_bytes = 0;
    for (std::uint16_t i = 0; i < record_count; ++i) {
        const unsigned char* header = data + 8 + static_cast<std::size_t>(i) * 18u;
        ParsedRecordHeader record;
        record.id = ReadLeUint(header, 8);
        record.lod_level = header[8];
        record.flags = header[9];
        record.uncompressed_size = static_cast<std::uint32_t>(ReadLeUint(header + 10, 4));
        record.compressed_size = static_cast<std::uint32_t>(ReadLeUint(header + 14, 4));
        payload_bytes += record.compressed_size;
        parsed.records.push_back(record);
    }
    EXPECT_EQ(bytes.size(), 8u + record_count * 18u + payload_bytes);
    return parsed;
}

} // namespace

TEST(WorldSaveRegionFormat, RegionAddressingUsesFloorDivision) {
    int rx = 99;
    int rz = 99;
    WorldSaveService::region_coords_for_chunk(IVec3(0, 5, 0), rx, rz);
    EXPECT_EQ(rx, 0);
    EXPECT_EQ(rz, 0);
    WorldSaveService::region_coords_for_chunk(IVec3(31, 0, 31), rx, rz);
    EXPECT_EQ(rx, 0);
    EXPECT_EQ(rz, 0);
    WorldSaveService::region_coords_for_chunk(IVec3(32, 0, 63), rx, rz);
    EXPECT_EQ(rx, 1);
    EXPECT_EQ(rz, 1);
    WorldSaveService::region_coords_for_chunk(IVec3(-1, 0, -32), rx, rz);
    EXPECT_EQ(rx, -1);
    EXPECT_EQ(rz, -1);
    WorldSaveService::region_coords_for_chunk(IVec3(-33, 0, -64), rx, rz);
    EXPECT_EQ(rx, -2);
    EXPECT_EQ(rz, -2);
}

TEST(WorldSaveRegionFormat, ChunkIdDecodeRoundTripsSignedPackedCoordinates) {
    const std::array<IVec3, 8> coordinates{{
        IVec3(0, 0, 0),
        IVec3(31, 7, -32),
        IVec3(-1, -1, -1),
        IVec3(1048575, 2097151, 1048575),
        IVec3(-1048576, -2097152, -1048576),
        IVec3(-32, 0, -27),
        IVec3(32, -9, 5),
        IVec3(-999, 12345, 777),
    }};
    for (const IVec3& coords : coordinates) {
        const IVec3 decoded = Chunk::decode_id(Chunk::calculate_id(coords));
        EXPECT_EQ(decoded.x, coords.x);
        EXPECT_EQ(decoded.y, coords.y);
        EXPECT_EQ(decoded.z, coords.z);
    }
}

TEST(WorldSaveRegionFormat, SaveWritesLmr1RegionFilesAndManifest) {
    TempSaveDir save_dir("layout");
    WorldSaveService service;

    WorldStreamingState state;
    PopulateMultiRegionWorld(state);
    auto edited = state.find_chunk(IVec3(0, 0, 0));
    ASSERT_NE(edited, nullptr);
    edited->mark_voxel_data_dirty();

    std::vector<std::string> errors;
    ASSERT_TRUE(service.save_world(state, save_dir.path, &errors));
    EXPECT_TRUE(errors.empty());

    const std::filesystem::path region_00 = WorldSaveService::region_file_path(save_dir.path, 0, 0);
    const std::filesystem::path region_10 = WorldSaveService::region_file_path(save_dir.path, 1, 0);
    const std::filesystem::path region_nn =
        WorldSaveService::region_file_path(save_dir.path, -1, -1);
    ASSERT_TRUE(std::filesystem::exists(region_00));
    ASSERT_TRUE(std::filesystem::exists(region_10));
    ASSERT_TRUE(std::filesystem::exists(region_nn));
    EXPECT_EQ(region_00.filename().string(), "r.0.0.lmr");
    EXPECT_EQ(region_nn.filename().string(), "r.-1.-1.lmr");

    const ParsedRegionFile parsed = ParseRegionFile(region_00);
    EXPECT_EQ(parsed.version, 2u);
    ASSERT_EQ(parsed.records.size(), 2u);
    for (const ParsedRecordHeader& record : parsed.records) {
        EXPECT_EQ(record.lod_level, 0u);
        EXPECT_GT(record.uncompressed_size, 0u);
        EXPECT_GT(record.compressed_size, 0u);
        // LZ4 must actually compress the JSON payloads.
        EXPECT_LT(record.compressed_size, record.uncompressed_size);
        // Both fixture chunks carry water sim state -> water-present (bit1).
        EXPECT_NE(record.flags & 0x02u, 0u);
    }
    // The edited chunk record carries the edited/authoritative flag (bit0);
    // the pristine one does not.
    const std::uint64_t edited_id = edited->get_id();
    int edited_records = 0;
    for (const ParsedRecordHeader& record : parsed.records) {
        if (record.id == edited_id) {
            EXPECT_NE(record.flags & 0x01u, 0u);
            ++edited_records;
        } else {
            EXPECT_EQ(record.flags & 0x01u, 0u);
        }
    }
    EXPECT_EQ(edited_records, 1);
    // Record ids are sorted ascending within a lod level.
    EXPECT_LT(parsed.records[0].id, parsed.records[1].id);

    const std::filesystem::path manifest_path =
        WorldSaveService::world_manifest_path(save_dir.path);
    ASSERT_TRUE(std::filesystem::exists(manifest_path));
    const std::string manifest = ReadFileBytes(manifest_path);
    EXPECT_NE(manifest.find("luminumbra.persistence.world_manifest.v1"), std::string::npos);
    EXPECT_NE(manifest.find("\"container\": \"LMR1\""), std::string::npos);
    EXPECT_EQ(manifest.find("next_durable_entity_id"), std::string::npos);

    // No legacy v1 snapshot is ever written by the v2 writer.
    EXPECT_FALSE(std::filesystem::exists(WorldSaveService::world_state_path(save_dir.path)));
}

TEST(WorldSaveRegionFormat, OldSnapshotRefusedAndCurrentContainerRoundTrips) {
    WorldSaveService service;
    WorldStreamingState original;
    PopulateMultiRegionWorld(original);
    TempSaveDir old_dir("old");
    WriteV1SnapshotFile(original, old_dir.path);
    WorldStreamingState loaded;
    loaded.insert_chunk(std::make_shared<Chunk>(IVec3(1, 0, 1)));
    std::vector<std::string> errors;
    EXPECT_FALSE(service.load_world(loaded, old_dir.path, errors));
    EXPECT_TRUE(loaded.empty());
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors.front(), WorldSaveService::kObsoleteWorldMessage);
    TempSaveDir current_dir("current");
    errors.clear();
    ASSERT_TRUE(service.save_world(original, current_dir.path, &errors));
    ASSERT_TRUE(service.load_world(loaded, current_dir.path, errors));
    EXPECT_EQ(service.world_hash(loaded), service.world_hash(original));
}

TEST(WorldSaveRegionFormat, SaveRefusesOldSnapshotWithoutRenamingOrOverwriting) {
    TempSaveDir save_dir("refusal");
    WorldSaveService service;
    WorldStreamingState original;
    PopulateMultiRegionWorld(original);
    WriteV1SnapshotFile(original, save_dir.path);
    const auto path = WorldSaveService::world_state_path(save_dir.path);
    const auto bytes = ReadFileBytes(path);
    std::vector<std::string> errors;
    EXPECT_FALSE(service.save_world(original, save_dir.path, &errors));
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors.front(), WorldSaveService::kObsoleteWorldMessage);
    EXPECT_EQ(ReadFileBytes(path), bytes);
    EXPECT_FALSE(std::filesystem::exists(path.string() + ".bak"));
    EXPECT_FALSE(std::filesystem::exists(WorldSaveService::region_directory(save_dir.path)));
}

TEST(WorldSaveRegionFormat, SaveDirtyChunksRewritesOnlyDirtyRegions) {
    TempSaveDir save_dir("incremental");
    WorldSaveService service;

    WorldStreamingState state;
    PopulateMultiRegionWorld(state);
    std::vector<std::string> errors;
    ASSERT_TRUE(service.save_world(state, save_dir.path, &errors));

    const std::filesystem::path region_00 = WorldSaveService::region_file_path(save_dir.path, 0, 0);
    const std::filesystem::path region_10 = WorldSaveService::region_file_path(save_dir.path, 1, 0);
    const std::filesystem::path region_nn =
        WorldSaveService::region_file_path(save_dir.path, -1, -1);
    const std::string region_10_before = ReadFileBytes(region_10);
    const std::string region_nn_before = ReadFileBytes(region_nn);
    const std::string region_00_before = ReadFileBytes(region_00);

    // Edit one chunk in region (0,0) only.
    auto edited = state.find_chunk(IVec3(1, 2, 3));
    ASSERT_NE(edited, nullptr);
    edited->sdf_data[0] = -64.0f;
    edited->mark_voxel_data_dirty();

    const auto report = service.save_dirty_chunks(state, save_dir.path, &errors);
    EXPECT_TRUE(errors.empty());
    EXPECT_TRUE(report.saved);
    EXPECT_EQ(report.chunks_dirty, 1u);
    EXPECT_EQ(report.regions_written, 1u);
    EXPECT_FALSE(edited->is_voxel_data_dirty());

    // Untouched regions are byte-identical; the dirty region changed.
    EXPECT_EQ(ReadFileBytes(region_10), region_10_before);
    EXPECT_EQ(ReadFileBytes(region_nn), region_nn_before);
    EXPECT_NE(ReadFileBytes(region_00), region_00_before);

    // The reload carries the edit.
    WorldStreamingState restored;
    std::vector<std::string> load_errors;
    ASSERT_TRUE(service.load_world(restored, save_dir.path, load_errors));
    const auto restored_chunk = restored.find_chunk(IVec3(1, 2, 3));
    ASSERT_NE(restored_chunk, nullptr);
    ASSERT_FALSE(restored_chunk->sdf_data.empty());
    EXPECT_EQ(restored_chunk->sdf_data[0], -64.0f);
    EXPECT_EQ(service.world_hash(restored), service.world_hash(state));
}

TEST(WorldSaveRegionFormat, IncrementalSavePreservesOnDiskChunksAbsentFromMemory) {
    TempSaveDir save_dir("merge");
    WorldSaveService service;

    // Two chunks in the SAME region persisted, then one is "unloaded" (saved
    // from a state that no longer contains it): its record must survive.
    WorldStreamingState full;
    AddFixtureChunk(full, IVec3(0, 0, 0), 1u);
    AddFixtureChunk(full, IVec3(5, 1, 7), 2u);
    std::vector<std::string> errors;
    ASSERT_TRUE(service.save_world(full, save_dir.path, &errors));

    WorldStreamingState partial;
    auto resident = AddFixtureChunk(partial, IVec3(0, 0, 0), 1u);
    resident->sdf_data[1] = 17.5f;
    resident->mark_voxel_data_dirty();
    const auto report = service.save_dirty_chunks(partial, save_dir.path, &errors);
    EXPECT_TRUE(errors.empty());
    EXPECT_TRUE(report.saved);

    WorldStreamingState restored;
    std::vector<std::string> load_errors;
    ASSERT_TRUE(service.load_world(restored, save_dir.path, load_errors));
    EXPECT_EQ(restored.size(), 2u);
    const auto unloaded = restored.find_chunk(IVec3(5, 1, 7));
    ASSERT_NE(unloaded, nullptr);
    EXPECT_FALSE(unloaded->sdf_data.empty());
    const auto edited = restored.find_chunk(IVec3(0, 0, 0));
    ASSERT_NE(edited, nullptr);
    EXPECT_EQ(edited->sdf_data[1], 17.5f);
}

TEST(WorldSaveRegionFormat,
     InterruptedChunkRewriteLeavesPriorRegionCompleteAndRetryPreservesOtherRecords) {
    TempSaveDir save_dir("atomic_chunk_rewrite");
    WorldSaveService service;

    // Persist two records in one region, then rewrite from a state containing
    // only one of them. The absent record exercises the raw-record merge path.
    WorldStreamingState full;
    auto original_target = AddFixtureChunk(full, IVec3(0, 0, 0), 1u);
    auto untargeted = AddFixtureChunk(full, IVec3(5, 1, 7), 2u);
    std::vector<std::string> errors;
    ASSERT_TRUE(service.save_world(full, save_dir.path, &errors));
    ASSERT_TRUE(errors.empty());

    const std::filesystem::path region = WorldSaveService::region_file_path(save_dir.path, 0, 0);
    const std::string prior_region_bytes = ReadFileBytes(region);
    ASSERT_FALSE(prior_region_bytes.empty());

    std::vector<WorldSaveService::ContainerRecord> records_before;
    ASSERT_TRUE(WorldSaveService::read_container_records(region, records_before, &errors));
    const auto untargeted_before =
        std::find_if(records_before.begin(),
                     records_before.end(),
                     [id = untargeted->get_id()](const WorldSaveService::ContainerRecord& record) {
                         return record.lod_level == 0u && record.id == id;
                     });
    ASSERT_NE(untargeted_before, records_before.end());
    const std::string untargeted_payload_before = untargeted_before->payload;
    const Luminumbra::u8 untargeted_flags_before = untargeted_before->flags;

    WorldStreamingState partial;
    auto updated_target = AddFixtureChunk(partial, IVec3(0, 0, 0), 1u);
    updated_target->sdf_data[0] = -64.0f;
    updated_target->mark_voxel_data_dirty();

    WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    const auto interrupted = service.save_dirty_chunks(partial, save_dir.path, &errors);
    WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
    EXPECT_FALSE(interrupted.saved);
    EXPECT_EQ(interrupted.regions_written, 0u);
    EXPECT_FALSE(errors.empty());
    EXPECT_TRUE(updated_target->is_voxel_data_dirty());
    EXPECT_EQ(ReadFileBytes(region), prior_region_bytes);
    EXPECT_FALSE(HasRegionTemporaryFile(region));

    // The live path is still a complete, loadable image of the prior region.
    WorldStreamingState after_interruption;
    std::vector<std::string> load_errors;
    ASSERT_TRUE(service.load_world(after_interruption, save_dir.path, load_errors));
    ASSERT_TRUE(load_errors.empty());
    ASSERT_EQ(after_interruption.size(), 2u);
    const auto old_target = after_interruption.find_chunk(original_target->get_id());
    ASSERT_NE(old_target, nullptr);
    ASSERT_FALSE(old_target->sdf_data.empty());
    EXPECT_EQ(old_target->sdf_data[0], -2.0f);
    EXPECT_NE(after_interruption.find_chunk(untargeted->get_id()), nullptr);

    // A normal retry commits the target edit while retaining the absent
    // chunk's record byte-for-byte at the uncompressed container boundary.
    errors.clear();
    const auto retried = service.save_dirty_chunks(partial, save_dir.path, &errors);
    ASSERT_TRUE(retried.saved);
    ASSERT_TRUE(errors.empty());
    EXPECT_FALSE(updated_target->is_voxel_data_dirty());
    EXPECT_NE(ReadFileBytes(region), prior_region_bytes);

    std::vector<WorldSaveService::ContainerRecord> records_after;
    ASSERT_TRUE(WorldSaveService::read_container_records(region, records_after, &errors));
    const auto untargeted_after =
        std::find_if(records_after.begin(),
                     records_after.end(),
                     [id = untargeted->get_id()](const WorldSaveService::ContainerRecord& record) {
                         return record.lod_level == 0u && record.id == id;
                     });
    ASSERT_NE(untargeted_after, records_after.end());
    EXPECT_EQ(untargeted_after->payload, untargeted_payload_before);
    EXPECT_EQ(untargeted_after->flags, untargeted_flags_before);

    WorldStreamingState after_retry;
    load_errors.clear();
    ASSERT_TRUE(service.load_world(after_retry, save_dir.path, load_errors));
    ASSERT_EQ(after_retry.size(), 2u);
    const auto committed_target = after_retry.find_chunk(updated_target->get_id());
    ASSERT_NE(committed_target, nullptr);
    ASSERT_FALSE(committed_target->sdf_data.empty());
    EXPECT_EQ(committed_target->sdf_data[0], -64.0f);
    EXPECT_NE(after_retry.find_chunk(untargeted->get_id()), nullptr);
}

TEST(WorldSaveRegionFormat, EmptySdfBandChunksRoundTrip) {
    // coarse (step>1) chunks carry empty or face-band-only SDF; the
    // container must persist and restore them without breaking the LOD0
    // promotion contract (heightmap intact, sdf restored verbatim).
    TempSaveDir save_dir("empty_sdf");
    WorldSaveService service;

    WorldStreamingState state;
    auto coarse = state.get_or_create_chunk(IVec3(2, 1, 2));
    coarse->set_state(ChunkState::Ready);
    coarse->sdf_data.clear(); // empty SDF ( coarse chunk)
    coarse->heightmap_data = {20.0f, 20.5f, 21.0f, 21.5f};
    coarse->current_lod.store(2, std::memory_order_release);
    coarse->mark_voxel_data_dirty();

    std::vector<std::string> errors;
    ASSERT_TRUE(service.save_world(state, save_dir.path, &errors));
    EXPECT_TRUE(errors.empty());

    WorldStreamingState restored;
    std::vector<std::string> load_errors;
    ASSERT_TRUE(service.load_world(restored, save_dir.path, load_errors));
    EXPECT_TRUE(load_errors.empty());
    const auto chunk = restored.find_chunk(IVec3(2, 1, 2));
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(chunk->sdf_data.empty());
    EXPECT_EQ(chunk->heightmap_data.size(), 4u);
    EXPECT_EQ(chunk->current_lod.load(std::memory_order_acquire), 2);
    EXPECT_EQ(service.world_hash(restored), service.world_hash(state));
}

TEST(WorldSaveRegionFormat, CorruptRegionFileReportsErrors) {
    TempSaveDir save_dir("corrupt");
    WorldSaveService service;

    const std::filesystem::path region_path =
        WorldSaveService::region_file_path(save_dir.path, 0, 0);
    std::filesystem::create_directories(region_path.parent_path());
    {
        std::ofstream output(region_path, std::ios::binary | std::ios::trunc);
        output << "this is not an LMR1 container";
    }

    WorldStreamingState state;
    std::vector<std::string> errors;
    EXPECT_FALSE(service.load_world(state, save_dir.path, errors));
    EXPECT_FALSE(errors.empty());
}

TEST(WorldSaveRegionFormat, HasWorldSaveRecognizesOnlySupportedContainers) {
    TempSaveDir fresh("detect_fresh");
    EXPECT_FALSE(WorldSaveService::has_world_save(fresh.path));

    TempSaveDir v1_dir("detect_v1");
    WorldStreamingState state;
    PopulateMultiRegionWorld(state);
    WriteV1SnapshotFile(state, v1_dir.path);
    EXPECT_FALSE(WorldSaveService::has_world_save(v1_dir.path));

    TempSaveDir v2_dir("detect_v2");
    WorldSaveService service;
    std::vector<std::string> errors;
    ASSERT_TRUE(service.save_world(state, v2_dir.path, &errors));
    EXPECT_TRUE(WorldSaveService::has_world_save(v2_dir.path));
}
