//  persistence v2: LMR1 region container round-trip, obsolete-format refusal
// (world-hash equality is the migration gate),.bak retention, and
// O(edited regions) incremental saves. Container spec:
// design decisions, section 3.
#include "gtest/gtest.h"

#include "../support/PersistenceFileOps.h"
#include "../support/UniqueTempPath.h"
#include "persistence/SaveTransaction.h"

#include "ecs/EntitySnapshot.h"
#include "persistence/SavedJsonAdmission.h"
#include "persistence/WorldPersistenceRoundtrip.h"
#include "persistence/WorldSaveService.h"
#include "world/ActiveRegionLedger.h"
#include "world/WorldClock.h"
#include "world/WorldStreamingState.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <stop_token>
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
    const std::filesystem::path dir =
        Luminumbra::TestSupport::CreateUniqueTempDirectory("luminumbra_region_format_" + tag + "_");

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

// Cancel at a real open boundary, without scheduling or filesystem timing assumptions.
class CancelSavedRead {
public:
    explicit CancelSavedRead(const std::filesystem::path& path,
                             bool at_close = false,
                             unsigned occurrence = 1)
        : m_path(path)
        , m_at_close(at_close)
        , m_occurrence(occurrence) {
        Active() = this;
        Luminumbra::Persistence::detail::set_file_read_observer_for_testing(&Observe);
    }
    ~CancelSavedRead() {
        Luminumbra::Persistence::detail::set_file_read_observer_for_testing(nullptr);
        Active() = nullptr;
    }
    CancelSavedRead(const CancelSavedRead&) = delete;
    CancelSavedRead& operator=(const CancelSavedRead&) = delete;

    std::stop_token token() const {
        return m_stop.get_token();
    }
    unsigned opens = 0;
    unsigned closes = 0;

private:
    static CancelSavedRead*& Active() {
        static CancelSavedRead* active = nullptr;
        return active;
    }
    static void Observe(const std::filesystem::path& path, bool opening) {
        auto* active = Active();
        if (!active || path != active->m_path)
            return;
        if (opening) {
            ++active->opens;
        } else {
            ++active->closes;
        }
        if ((active->m_at_close ? !opening : opening) &&
            (active->m_at_close ? active->closes : active->opens) == active->m_occurrence)
            active->m_stop.request_stop();
    }
    std::filesystem::path m_path;
    const bool m_at_close;
    const unsigned m_occurrence;
    std::stop_source m_stop;
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
    // Relocated staging lives in the save root, outside the strict region scan.
    EXPECT_FALSE(HasRegionTemporaryFile(save_dir.path / region.filename()));

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

TEST(WorldSaveRegionFormat, MaximalCompressedSizeIsRefusedAsTruncated) {
    TempSaveDir save_dir("payload_bounds");
    const auto path = WorldSaveService::region_file_path(save_dir.path, 0, 0);
    std::filesystem::create_directories(path.parent_path());

    // Oversized declarations fail admission before any payload allocation.
    // An admitted declaration still reaches the truncated-file diagnostic.
    // UINT32_MAX also guards against offset + size wrapping on 32-bit hosts.
    for (const auto compressed_size :
         {std::numeric_limits<std::uint32_t>::max(), std::uint32_t{1}}) {
        SCOPED_TRACE(compressed_size);
        std::string bytes("LMR1\x02\x00\x01\x00", 8);
        bytes.append(18, '\0');
        bytes[8 + 10] = 1;
        for (std::size_t i = 0; i < 4; ++i)
            bytes[8 + 14 + i] = static_cast<char>(compressed_size >> (8 * i));
        ASSERT_TRUE(WorldSaveService::write_file_durably(path, bytes));
        std::vector<WorldSaveService::ContainerRecord> records;
        std::vector<std::string> errors;
        EXPECT_FALSE(WorldSaveService::read_container_records(path, records, &errors));
        EXPECT_TRUE(records.empty());
        ASSERT_EQ(errors.size(), 1u);
        EXPECT_NE(errors.front().find(compressed_size == 1 ? "record payload is truncated"
                                                           : "compatibility limit"),
                  std::string::npos);
        EXPECT_EQ(ReadFileBytes(path), bytes);
    }
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

TEST(WorldSaveRegionFormat, ExactIndexedReadDistinguishesAbsentFromLostOrCorruptAuthority) {
    namespace P = Luminumbra::Persistence;
    TempSaveDir save("exact_indexed");
    WorldStreamingState state;
    auto chunk = state.get_or_create_chunk({40, 0, -40});
    chunk->sdf_data.assign(17 * 17 * 17, -2.0f);
    chunk->mark_voxel_data_dirty();
    {
        P::SaveTransaction transaction(save.path, 1, {}, &WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(WorldSaveService{}.save_world(state, save.path));
        ASSERT_TRUE(transaction.commit(nullptr));
    }
    const auto found = WorldSaveService::read_chunk(save.path, {40, 0, -40}, true);
    ASSERT_EQ(found.status, WorldSaveService::ChunkReadStatus::Found);
    ASSERT_NE(found.chunk, nullptr);
    EXPECT_EQ(found.chunk->sdf_data, chunk->sdf_data);
    EXPECT_EQ(WorldSaveService::read_chunk(save.path, {41, 0, -40}, true).status,
              WorldSaveService::ChunkReadStatus::Absent);
    const auto region = WorldSaveService::region_file_path(save.path, 1, -2);
    const auto bytes = ReadFileBytes(region);
    ASSERT_TRUE(std::filesystem::remove(region));
    EXPECT_EQ(WorldSaveService::read_chunk(save.path, {40, 0, -40}, true).status,
              WorldSaveService::ChunkReadStatus::Error);
    std::vector<std::shared_ptr<Chunk>> region_chunks;
    EXPECT_FALSE(WorldSaveService::read_region_chunks(region, region_chunks));
    std::ofstream(region, std::ios::binary) << bytes;
    std::ofstream(region, std::ios::binary | std::ios::trunc) << "invalid";
    EXPECT_EQ(WorldSaveService::read_chunk(save.path, {40, 0, -40}, true).status,
              WorldSaveService::ChunkReadStatus::Error);
    ASSERT_TRUE(std::filesystem::remove(P::snapshot_index_path(save.path)));
    EXPECT_EQ(WorldSaveService::read_chunk(save.path, {0, 0, 0}, true).status,
              WorldSaveService::ChunkReadStatus::Error);
}

TEST(WorldSaveRegionFormat, IndexedCommitRefusesMalformedLatticeWithoutAcknowledgement) {
    namespace P = Luminumbra::Persistence;
    TempSaveDir save("indexed_lattice");
    WorldStreamingState state;
    auto chunk = state.get_or_create_chunk({0, 0, 0});
    chunk->sdf_data.assign(17 * 17 * 17, -1.0f);
    chunk->mark_voxel_data_dirty();
    {
        P::SaveTransaction transaction(save.path, 1, {}, &WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(WorldSaveService{}.save_world(state, save.path));
        ASSERT_TRUE(transaction.commit(nullptr));
    }
    chunk->sdf_data.resize(4);
    chunk->mark_voxel_data_dirty();
    {
        P::SaveTransaction transaction(save.path, 2, {}, &WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(nullptr));
        EXPECT_EQ(WorldSaveService{}.save_dirty_chunks(state, save.path).regions_written, 1u);
        EXPECT_FALSE(transaction.commit(nullptr));
    }
    EXPECT_TRUE(chunk->is_voxel_data_dirty());
    const auto recovered = WorldSaveService::read_chunk(save.path, {0, 0, 0}, true);
    ASSERT_EQ(recovered.status, WorldSaveService::ChunkReadStatus::Found);
    ASSERT_NE(recovered.chunk, nullptr);
    EXPECT_EQ(recovered.chunk->sdf_data.size(), 17u * 17u * 17u);
}

TEST(WorldSaveRegionFormat, FilteredBootValidatesDistantLatticesAndPublishesInventoryAtomically) {
    TempSaveDir save("filtered_inventory");
    WorldStreamingState source;
    for (const IVec3 coords : {IVec3(0, 0, 0), IVec3(400, 0, 400)}) {
        auto chunk = source.get_or_create_chunk(coords);
        chunk->sdf_data.assign(17 * 17 * 17, -1.0f);
        chunk->mark_voxel_data_dirty();
    }
    ASSERT_TRUE(WorldSaveService{}.save_world(source, save.path));
    WorldStreamingState loaded;
    std::vector<IVec3> inventory;
    std::vector<std::string> errors;
    const std::unordered_set<Luminumbra::ChunkID> wanted{Chunk::calculate_id({0, 0, 0})};
    ASSERT_TRUE(WorldSaveService{}.load_world(loaded, save.path, errors, {}, &wanted, &inventory));
    EXPECT_EQ(loaded.size(), 1u);
    EXPECT_EQ(inventory.size(), 2u);
    source.find_chunk(Chunk::calculate_id({400, 0, 400}))->sdf_data.resize(4);
    ASSERT_TRUE(WorldSaveService{}.save_world(source, save.path));
    EXPECT_FALSE(WorldSaveService{}.load_world(loaded, save.path, errors, {}, &wanted, &inventory));
    EXPECT_EQ(loaded.size(), 0u);
    EXPECT_TRUE(inventory.empty());
    ASSERT_FALSE(errors.empty());
    EXPECT_NE(errors.back().find("SDF lattice"), std::string::npos);
}

TEST(WorldSaveRegionFormat, JoinedIncrementalSaveAcknowledgesOnlyCommittedUnchangedRevisions) {
    namespace P = Luminumbra::Persistence;
    TempSaveDir save("joined_ack");
    WorldStreamingState state;
    auto chunk = state.get_or_create_chunk({0, 0, 0});
    chunk->sdf_data.assign(17 * 17 * 17, 1.0f);
    chunk->mark_voxel_data_dirty();
    chunk->mark_water_state_dirty();
    ASSERT_TRUE(WorldSaveService{}.save_world(state, save.path));
    for (const int mode : {0, 1, 2}) {
        SCOPED_TRACE(mode);
        chunk->mark_voxel_data_dirty();
        chunk->mark_water_state_dirty();
        {
            P::SaveTransaction transaction(save.path, 1, {}, &WorldSaveService::member_digest);
            ASSERT_TRUE(transaction.begin(nullptr));
            EXPECT_EQ(WorldSaveService{}.save_dirty_chunks(state, save.path).regions_written, 1u);
            EXPECT_TRUE(chunk->is_voxel_data_dirty());
            EXPECT_TRUE(chunk->is_water_state_dirty());
            if (mode == 1) {
                chunk->sdf_data[0] = -3.0f;
                chunk->mark_voxel_data_dirty();
                chunk->mark_water_state_dirty();
            }
            if (mode != 0) {
                ASSERT_TRUE(transaction.commit(nullptr));
            }
        }
        EXPECT_EQ(chunk->is_voxel_data_dirty(), mode != 2);
        EXPECT_EQ(chunk->is_water_state_dirty(), mode != 2);
    }
}

TEST(WorldSaveRegionFormat, MetadataByteAndTextCompatibilityBoundariesPreserveExistingFiles) {
    using namespace Luminumbra::Persistence;
    TempSaveDir directory("metadata_limits");
    const auto path = directory.path / "world_info.json";
    auto write = [&path](const std::string& bytes) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << bytes;
        return output.good();
    };
    const std::string base = R"({"container_version":2})";
    std::string exact = base;
    exact.resize(SavedInputLimits::kMetadataBytes, ' ');
    ASSERT_TRUE(WorldSaveService::save_metadata(exact, directory.path));
    Luminumbra::world::WorldClock clock;
    bool required = false;
    ASSERT_TRUE(WorldSaveService::read_clock_metadata(directory.path, clock, required));
    EXPECT_EQ(ReadFileBytes(path), exact);
    exact += ' ';
    EXPECT_FALSE(WorldSaveService::save_metadata(exact, directory.path));
    EXPECT_EQ(ReadFileBytes(path).size(), SavedInputLimits::kMetadataBytes);
    ASSERT_TRUE(write(exact));
    EXPECT_FALSE(WorldSaveService::read_clock_metadata(directory.path, clock, required));
    WorldStreamingState state;
    std::vector<std::string> errors;
    EXPECT_FALSE(WorldSaveService{}.load_world(state, directory.path, errors));
    EXPECT_FALSE(errors.empty());
    ASSERT_TRUE(write(base));
    for (const auto* key : {"name", "seed"}) {
        const auto maximum = std::string_view(key) == "name" ? SavedInputLimits::kWorldNameBytes
                                                             : SavedInputLimits::kWorldSeedBytes;
        nlohmann::json metadata = {{"container_version", 2}, {key, std::string(maximum, 'x')}};
        ASSERT_TRUE(WorldSaveService::save_metadata(metadata.dump(), directory.path));
        EXPECT_TRUE(WorldSaveService::read_clock_metadata(directory.path, clock, required));
        const auto accepted = ReadFileBytes(path);
        metadata[key] = std::string(maximum + 1, 'x');
        EXPECT_FALSE(WorldSaveService::save_metadata(metadata.dump(), directory.path));
        EXPECT_EQ(ReadFileBytes(path), accepted);
        ASSERT_TRUE(write(metadata.dump()));
        EXPECT_FALSE(WorldSaveService::read_clock_metadata(directory.path, clock, required));
        ASSERT_TRUE(write(base));
    }
}

TEST(WorldSaveRegionFormat, NormalizedMetadataLimitIsAdmittedBeforeEitherPairedWrite) {
    using namespace Luminumbra::Persistence;
    using Luminumbra::world::ActiveRegionLedger;
    using Luminumbra::world::WorldCalendar;
    using Luminumbra::world::WorldClock;
    for (const bool over : {false, true}) {
        TempSaveDir directory("normalized_metadata_limits");
        ActiveRegionLedger installed_ledger;
        ASSERT_TRUE(installed_ledger.try_set_local_anchor(Vec3{}));
        nlohmann::json installed = {{"container_version", 2}};
        WorldClock(100, WorldCalendar::Legacy()).write_metadata(installed);
        ASSERT_TRUE(WorldSaveService::save_metadata_and_active_regions(
            installed.dump(), installed_ledger, directory.path));
        const auto metadata_path = directory.path / "world_info.json";
        const auto ledger_path = WorldSaveService::active_regions_path(directory.path);
        const auto before_metadata = ReadFileBytes(metadata_path);
        const auto before_ledger = ReadFileBytes(ledger_path);
        ActiveRegionLedger ledger;
        ASSERT_TRUE(ledger.try_set_local_anchor(Vec3{}));
        const auto before_live = ledger.encode();

        nlohmann::json candidate = {
            {"container_version", 2}, {"calendar", installed.at("calendar")}, {"extension", ""}};
        auto normalized = candidate;
        WorldClock(0, WorldCalendar::Legacy()).write_metadata(normalized);
        const auto base_bytes = normalized.dump(4).size() + 1;
        candidate["extension"] =
            std::string(SavedInputLimits::kMetadataBytes + (over ? 1 : 0) - base_bytes, 'x');
        const auto bytes = candidate.dump(4) + "\n";
        ASSERT_LT(bytes.size(), SavedInputLimits::kMetadataBytes);
        EXPECT_EQ(WorldSaveService::save_metadata_and_active_regions(bytes, ledger, directory.path),
                  !over);
        if (over) {
            EXPECT_EQ(ReadFileBytes(metadata_path), before_metadata);
            EXPECT_EQ(ReadFileBytes(ledger_path), before_ledger);
            EXPECT_EQ(ledger.encode(), before_live);
        } else {
            EXPECT_EQ(ReadFileBytes(metadata_path).size(), SavedInputLimits::kMetadataBytes);
            WorldClock loaded;
            bool required = false;
            EXPECT_TRUE(WorldSaveService::read_clock_metadata(directory.path, loaded, required));
            EXPECT_EQ(loaded.tick(), 0u);
        }
    }
}

TEST(WorldSaveRegionFormat, JsonAdmissionDistinguishesLimitsFromMalformedSavedData) {
    using namespace Luminumbra::Persistence;
    TempSaveDir directory("json_refusal_diagnostics");
    const auto path = directory.path / "world_info.json";
    nlohmann::json output = {{"unchanged", true}};
    const auto original = output;
    bool missing = false;
    SavedJsonFailure failure = SavedJsonFailure::Malformed;
    EXPECT_TRUE(
        ReadSavedJson(path, SavedInputLimits::kMetadataBytes, output, missing, true, &failure));
    EXPECT_TRUE(missing);
    EXPECT_EQ(failure, SavedJsonFailure::None);
    EXPECT_EQ(output, original);
    for (const bool oversized : {false, true}) {
        auto bytes = oversized ? std::string(SavedInputLimits::kMetadataBytes + 1, ' ') : "{";
        ASSERT_TRUE(WorldSaveService::write_file_durably(path, bytes));
        EXPECT_FALSE(
            ReadSavedJson(path, SavedInputLimits::kMetadataBytes, output, missing, true, &failure));
        EXPECT_FALSE(missing);
        EXPECT_EQ(failure, oversized ? SavedJsonFailure::Limit : SavedJsonFailure::Malformed);
        EXPECT_EQ(output, original);
    }
}

TEST(WorldSaveRegionFormat, CancellationAtJsonOpenPreservesAdmissionOutputsAndClosesTheHandle) {
    using namespace Luminumbra::Persistence;
    TempSaveDir directory("cancel_json_open");
    const auto path = directory.path / "world_info.json";
    // Exercise exact-limit, over-limit and malformed inputs under cancellation.
    std::string exact = "{}";
    exact.resize(SavedInputLimits::kMetadataBytes, ' ');
    for (const auto& bytes :
         {exact, std::string(SavedInputLimits::kMetadataBytes + 1, ' '), std::string("{")}) {
        ASSERT_TRUE(WorldSaveService::write_file_durably(path, bytes));
        CancelSavedRead cancellation(path);
        nlohmann::json value = {{"unchanged", true}};
        const auto original = value;
        bool missing = true;
        SavedJsonFailure failure = SavedJsonFailure::None;
        EXPECT_FALSE(ReadSavedJson(path,
                                   SavedInputLimits::kMetadataBytes,
                                   value,
                                   missing,
                                   true,
                                   &failure,
                                   cancellation.token()));
        EXPECT_EQ(failure, SavedJsonFailure::Cancelled);
        EXPECT_FALSE(missing);
        EXPECT_EQ(value, original);
        EXPECT_EQ(cancellation.opens, 1u);
        EXPECT_EQ(cancellation.closes, 1u);
        EXPECT_EQ(ReadFileBytes(path), bytes);
    }
    std::stop_source stopped;
    stopped.request_stop();
    for (const auto& refused_path : {directory.path / "absent", directory.path}) {
        nlohmann::json value = {{"unchanged", true}};
        const auto original = value;
        bool missing = true;
        SavedJsonFailure failure = SavedJsonFailure::None;
        EXPECT_FALSE(ReadSavedJson(refused_path,
                                   SavedInputLimits::kMetadataBytes,
                                   value,
                                   missing,
                                   true,
                                   &failure,
                                   stopped.get_token()));
        EXPECT_EQ(failure, SavedJsonFailure::Cancelled);
        EXPECT_FALSE(missing);
        EXPECT_EQ(value, original);
    }
}

TEST(WorldSaveRegionFormat, CancellationAtEachSavedPayloadOpenDiscardsStagedAuthority) {
    using namespace Luminumbra;
    using namespace Luminumbra::Persistence;
    TempSaveDir directory("cancel_payload_open");
    WorldStreamingState state;
    AddFixtureChunk(state, IVec3(0, 0, 0), 1);
    ASSERT_TRUE(WorldSaveService{}.save_world(state, directory.path));
    world::WorldClock clock(100);
    world::ActiveRegionLedger ledger({}, clock);
    // Empty anchorless ledgers intentionally have no file. Persist authority so
    // cancellation actually fires at this payload's open rather than a clean miss.
    ledger.set_local_anchor(Vec3(1, 2, 3));
    nlohmann::json metadata = {{"container_version", WorldSaveService::kContainerVersion}};
    clock.write_metadata(metadata);
    ASSERT_TRUE(WorldSaveService::save_metadata_and_active_regions(
        metadata.dump(), ledger, directory.path));
    Ecs::EntityRegistrySnapshot plants;
    plants.entities.push_back(
        Ecs::EntitySnapshotRecord{.entity_id = 1, .name = "plant", .components = {}});
    ASSERT_TRUE(WorldSaveService::save_plant_entities(plants, directory.path));
    for (const auto& path : {directory.path / "world_info.json",
                             WorldSaveService::world_manifest_path(directory.path),
                             WorldSaveService::region_file_path(directory.path, 0, 0),
                             WorldSaveService::active_regions_path(directory.path),
                             WorldSaveService::plant_entities_path(directory.path)}) {
        SCOPED_TRACE(path);
        ASSERT_TRUE(std::filesystem::is_regular_file(path));
        const auto before = ReadFileBytes(path);
        // Metadata is also opened for the clock/ledger pair comparison. All
        // other payloads open once. Exercise both reads and their close boundaries.
        const unsigned occurrences = path.filename() == "world_info.json" ? 2u : 1u;
        for (const bool at_close : {false, true}) {
            for (unsigned occurrence = 1; occurrence <= occurrences; ++occurrence) {
                SCOPED_TRACE(at_close);
                SCOPED_TRACE(occurrence);
                CancelSavedRead cancellation(path, at_close, occurrence);
                WorldStreamingState loaded;
                loaded.get_or_create_chunk(IVec3(1, 0, 1));
                std::vector<std::string> errors;
                EXPECT_FALSE(WorldSaveService{}.load_world(
                    loaded, directory.path, errors, cancellation.token()));
                EXPECT_TRUE(loaded.empty());
                ASSERT_FALSE(errors.empty());
                EXPECT_EQ(errors.front(), "Saved-world validation cancelled.");
                EXPECT_EQ(cancellation.opens, occurrence);
                EXPECT_EQ(cancellation.closes, occurrence);
                EXPECT_EQ(ReadFileBytes(path), before);
            }
        }
    }
}

TEST(WorldSaveRegionFormat, CancellationAtMetadataCloseWithoutPayloadsIsNotACleanMiss) {
    TempSaveDir directory("metadata_close_cancellation");
    const auto path = directory.path / "world_info.json";
    const std::string metadata = R"({"container_version":2})";
    ASSERT_TRUE(WorldSaveService::write_file_durably(path, metadata));
    CancelSavedRead cancellation(path, true);
    WorldStreamingState loaded;
    loaded.get_or_create_chunk(IVec3(1, 0, 1));
    std::vector<std::string> errors;
    EXPECT_FALSE(
        WorldSaveService{}.load_world(loaded, directory.path, errors, cancellation.token()));
    EXPECT_TRUE(loaded.empty());
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors.front(), "Saved-world validation cancelled.");
    EXPECT_EQ(cancellation.opens, 1u);
    EXPECT_EQ(cancellation.closes, 1u);
    EXPECT_EQ(ReadFileBytes(path), metadata);
    EXPECT_FALSE(std::filesystem::exists(directory.path / "chunks"));
}

TEST(WorldSaveRegionFormat, MissingPersistenceDirectoriesAreQuietButWrongTypesRefuse) {
    for (const bool chunks_leaf : {false, true}) {
        TempSaveDir directory("directory_admission");
        SCOPED_TRACE(chunks_leaf);
        const auto path = chunks_leaf ? directory.path / "chunks"
                                      : WorldSaveService::region_directory(directory.path);
        std::filesystem::create_directories(path.parent_path());
        WorldStreamingState loaded;
        std::vector<std::string> errors;
        EXPECT_FALSE(WorldSaveService{}.load_world(loaded, directory.path, errors));
        EXPECT_TRUE(loaded.empty());
        EXPECT_TRUE(errors.empty());
        ASSERT_TRUE(WorldSaveService::write_file_durably(path, "not a directory"));
        loaded.get_or_create_chunk(IVec3(1, 0, 1));
        EXPECT_FALSE(WorldSaveService{}.load_world(loaded, directory.path, errors));
        EXPECT_TRUE(loaded.empty());
        EXPECT_FALSE(errors.empty());
        EXPECT_EQ(ReadFileBytes(path), "not a directory");
    }
}

#if !defined(_WIN32)
TEST(WorldSaveRegionFormat, PersistenceDirectoryLinksAreRefusalsIncludingDanglingTargets) {
    for (const bool link_chunks : {false, true}) {
        for (const bool dangling : {false, true}) {
            SCOPED_TRACE(link_chunks);
            SCOPED_TRACE(dangling);
            TempSaveDir directory("linked_persistence_directory");
            const auto target = directory.path / "target";
            WorldStreamingState empty;
            ASSERT_TRUE(WorldSaveService{}.save_world(empty, target));
            const auto alias = directory.path / "alias";
            std::filesystem::create_directories(alias);
            const auto leaf =
                link_chunks ? alias / "chunks" : WorldSaveService::region_directory(alias);
            std::filesystem::create_directories(leaf.parent_path());
            const auto destination = dangling      ? directory.path / "absent"
                                     : link_chunks ? target / "chunks"
                                                   : WorldSaveService::region_directory(target);
            std::filesystem::create_directory_symlink(destination, leaf);
            const auto manifest = WorldSaveService::world_manifest_path(target);
            const auto before = ReadFileBytes(manifest);
            WorldStreamingState loaded;
            loaded.get_or_create_chunk(IVec3(1, 0, 1));
            std::vector<std::string> errors;
            EXPECT_FALSE(WorldSaveService{}.load_world(loaded, alias, errors));
            EXPECT_FALSE(errors.empty());
            EXPECT_TRUE(loaded.empty());
            EXPECT_TRUE(std::filesystem::is_symlink(leaf));
            EXPECT_EQ(ReadFileBytes(manifest), before);
        }
    }
}
TEST(WorldSaveRegionFormat, InternalDirectoryLinksPreservePayloadAdmissionAndCancellation) {
    // Both directory positions and both target spellings must resolve inside
    // the root. Leaf files, including plant rosters, keep strict admission.
    for (const bool link_chunks : {false, true}) {
        for (const bool relative_target : {false, true}) {
            TempSaveDir directory("internal_persistence_directory");
            SCOPED_TRACE(link_chunks);
            SCOPED_TRACE(relative_target);
            const auto storage = directory.path / "storage";
            const auto leaf = link_chunks ? directory.path / "chunks"
                                          : WorldSaveService::region_directory(directory.path);
            const auto target = link_chunks ? storage : storage / "region";
            std::filesystem::create_directories(target);
            std::filesystem::create_directories(leaf.parent_path());
            std::filesystem::create_directory_symlink(
                relative_target ? target.lexically_relative(leaf.parent_path()) : target, leaf);
            WorldStreamingState loaded;
            std::vector<std::string> errors;
            EXPECT_FALSE(WorldSaveService{}.load_world(loaded, directory.path, errors));
            EXPECT_TRUE(loaded.empty());
            EXPECT_TRUE(errors.empty());
            WorldStreamingState original;
            AddFixtureChunk(original, IVec3(0, 0, 0), 1);
            ASSERT_TRUE(WorldSaveService{}.save_world(original, directory.path));
            ASSERT_TRUE(WorldSaveService{}.load_world(loaded, directory.path, errors));
            EXPECT_TRUE(errors.empty());
            EXPECT_EQ(WorldSaveService{}.world_hash(loaded),
                      WorldSaveService{}.world_hash(original));
            const auto region = WorldSaveService::region_file_path(directory.path, 0, 0);
            {
                CancelSavedRead cancellation(region);
                EXPECT_FALSE(WorldSaveService{}.load_world(
                    loaded, directory.path, errors, cancellation.token()));
                EXPECT_TRUE(loaded.empty());
                ASSERT_FALSE(errors.empty());
                EXPECT_EQ(errors.front(), "Saved-world validation cancelled.");
                EXPECT_EQ(cancellation.opens, 1u);
                EXPECT_EQ(cancellation.closes, 1u);
            }
            // Partial payloads and unknown inventory in an allowed directory
            // remain refusals, with no destination rewrite on failed preflight.
            errors.clear();
            ASSERT_TRUE(WorldSaveService::write_file_durably(region, "LMR"));
            EXPECT_FALSE(WorldSaveService{}.load_world(loaded, directory.path, errors));
            EXPECT_FALSE(errors.empty());
            EXPECT_FALSE(WorldSaveService{}.save_world(original, directory.path));
            EXPECT_EQ(ReadFileBytes(region), "LMR");
            ASSERT_TRUE(std::filesystem::remove(region));
            const auto unknown = WorldSaveService::region_directory(directory.path) / "unknown";
            ASSERT_TRUE(WorldSaveService::write_file_durably(unknown, "unknown authority"));
            errors.clear();
            EXPECT_FALSE(WorldSaveService{}.load_world(loaded, directory.path, errors));
            EXPECT_FALSE(errors.empty());
            EXPECT_EQ(ReadFileBytes(unknown), "unknown authority");
        }
    }
}

TEST(WorldSaveRegionFormat, DirectoryLinkReplacementAtPayloadOpenDiscardsStagedAuthority) {
    class ReplaceDirectoryOnOpen {
    public:
        ReplaceDirectoryOnOpen(const std::filesystem::path& payload,
                               const std::filesystem::path& link,
                               const std::filesystem::path& replacement)
            : m_payload(payload)
            , m_link(link)
            , m_replacement(replacement) {
            Active() = this;
            Luminumbra::Persistence::detail::set_file_read_observer_for_testing(&Observe);
        }
        ~ReplaceDirectoryOnOpen() {
            Luminumbra::Persistence::detail::set_file_read_observer_for_testing(nullptr);
            Active() = nullptr;
        }
        ReplaceDirectoryOnOpen(const ReplaceDirectoryOnOpen&) = delete;
        ReplaceDirectoryOnOpen& operator=(const ReplaceDirectoryOnOpen&) = delete;
        bool replaced = false;

    private:
        static ReplaceDirectoryOnOpen*& Active() {
            static ReplaceDirectoryOnOpen* active = nullptr;
            return active;
        }
        static void Observe(const std::filesystem::path& path, bool opening) {
            auto* active = Active();
            if (!active || !opening || path != active->m_payload || active->replaced)
                return;
            std::filesystem::remove(active->m_link);
            std::filesystem::create_directory_symlink(active->m_replacement, active->m_link);
            active->replaced = true;
        }
        const std::filesystem::path m_payload;
        const std::filesystem::path m_link;
        const std::filesystem::path m_replacement;
    };
    // Internal replacement exercises identity mismatch; external and dangling
    // replacements exercise containment and failed resolution after discovery.
    for (const unsigned replacement_kind : {0u, 1u, 2u}) {
        SCOPED_TRACE(replacement_kind);
        TempSaveDir directory("directory_replacement");
        TempSaveDir outside("directory_replacement_external");
        WorldStreamingState original;
        AddFixtureChunk(original, IVec3(0, 0, 0), 1);
        ASSERT_TRUE(WorldSaveService{}.save_world(original, directory.path));
        const auto leaf = WorldSaveService::region_directory(directory.path);
        const auto initial = directory.path / "initial";
        std::filesystem::rename(leaf, initial);
        const auto replacement = replacement_kind == 0   ? directory.path / "replacement"
                                 : replacement_kind == 1 ? outside.path
                                                         : directory.path / "absent";
        if (replacement_kind != 2) {
            std::filesystem::create_directories(replacement);
            // The fixture has exactly one region and one manifest. A second
            // complete roster cannot hide an identity change behind equal bytes.
            for (const auto* filename : {"r.0.0.lmr", "world-manifest.json"})
                ASSERT_TRUE(WorldSaveService::write_file_durably(
                    replacement / filename, ReadFileBytes(initial / filename)));
        }
        std::filesystem::create_directory_symlink(initial, leaf);
        const auto region = WorldSaveService::region_file_path(directory.path, 0, 0);
        const auto before = ReadFileBytes(initial / region.filename());
        ReplaceDirectoryOnOpen replacing(region, leaf, replacement);
        WorldStreamingState loaded;
        std::vector<std::string> errors;
        EXPECT_FALSE(WorldSaveService{}.load_world(loaded, directory.path, errors));
        EXPECT_TRUE(replacing.replaced);
        EXPECT_TRUE(loaded.empty());
        EXPECT_FALSE(errors.empty());
        EXPECT_EQ(ReadFileBytes(initial / region.filename()), before);
        if (replacement_kind != 2) {
            EXPECT_EQ(ReadFileBytes(replacement / region.filename()), before);
        }
    }
}

TEST(WorldSaveRegionFormat, DirectoryLinksToWrongTypesAndEscapesRefuseWithoutPayloadReads) {
    for (const bool link_chunks : {false, true}) {
        TempSaveDir directory("unsafe_directory_target");
        const auto save_root = directory.path / "save";
        std::filesystem::create_directories(save_root);
        const auto leaf =
            link_chunks ? save_root / "chunks" : WorldSaveService::region_directory(save_root);
        std::filesystem::create_directories(leaf.parent_path());
        const auto file = save_root / "ordinary-file";
        ASSERT_TRUE(WorldSaveService::write_file_durably(file, "not a directory"));
        // A component-prefix sibling is outside the root too; a string-prefix
        // comparison must not admit it. Existing tests cover valid external saves.
        const auto outside = directory.path / "save-sibling";
        std::filesystem::create_directories(outside);
        for (const auto& target : {file,
                                   save_root / "absent",
                                   std::filesystem::path("/dev/zero"),
                                   outside,
                                   save_root,
                                   leaf}) {
            SCOPED_TRACE(link_chunks);
            SCOPED_TRACE(target);
            std::filesystem::create_directory_symlink(target, leaf);
            WorldStreamingState loaded;
            loaded.get_or_create_chunk(IVec3(1, 0, 1));
            std::vector<std::string> errors;
            EXPECT_FALSE(WorldSaveService{}.load_world(loaded, save_root, errors));
            EXPECT_FALSE(errors.empty());
            EXPECT_TRUE(loaded.empty());
            EXPECT_FALSE(WorldSaveService{}.save_world(loaded, save_root));
            // The same invalid parents must not turn an absent optional roster
            // or a raw record writer's missing input into a successful clean miss.
            Luminumbra::Ecs::EntityRegistrySnapshot plants;
            plants.entities.push_back(Luminumbra::Ecs::EntitySnapshotRecord{
                .entity_id = 1, .name = "unchanged", .components = {}});
            const auto original_plants =
                Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(plants);
            errors.clear();
            EXPECT_FALSE(WorldSaveService::load_plant_entities(plants, save_root, &errors));
            EXPECT_FALSE(errors.empty());
            EXPECT_EQ(Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(plants),
                      original_plants);
            const WorldSaveService::ContainerRecord record{
                .id = 1, .lod_level = 1, .flags = 0, .payload = "unpublished"};
            EXPECT_FALSE(WorldSaveService::upsert_container_records(
                WorldSaveService::region_file_path(save_root, 0, 0), {record}));
            EXPECT_TRUE(std::filesystem::is_empty(outside));
            EXPECT_TRUE(std::filesystem::is_symlink(leaf));
            ASSERT_TRUE(std::filesystem::remove(leaf));
        }
    }
}

#endif

TEST(WorldSaveRegionFormat, ManifestExactCompatibilityLimitLoadsButOneMoreByteRefuses) {
    using namespace Luminumbra::Persistence;
    TempSaveDir directory("manifest_limits");
    WorldStreamingState empty;
    ASSERT_TRUE(WorldSaveService{}.save_world(empty, directory.path));
    const auto path = WorldSaveService::world_manifest_path(directory.path);
    auto bytes = ReadFileBytes(path);
    bytes.resize(SavedInputLimits::kManifestBytes, ' ');
    ASSERT_TRUE(WorldSaveService::write_file_durably(path, bytes));
    std::vector<std::string> errors;
    EXPECT_TRUE(WorldSaveService{}.load_world(empty, directory.path, errors));
    bytes += ' ';
    ASSERT_TRUE(WorldSaveService::write_file_durably(path, bytes));
    EXPECT_FALSE(WorldSaveService{}.load_world(empty, directory.path, errors));
    EXPECT_FALSE(errors.empty());
    EXPECT_EQ(ReadFileBytes(path), bytes);
}

TEST(WorldSaveRegionFormat, RegionPayloadWriterAndReaderShareThe64MiBLimit) {
    using namespace Luminumbra::Persistence;
    TempSaveDir directory("record_limits");
    const auto path = directory.path / "container.lmr";
    WorldSaveService::ContainerRecord record{
        .id = 1,
        .lod_level = 1,
        .flags = 0,
        .payload = std::string(SavedInputLimits::kRegionRecordBytes, 'x')};
    ASSERT_TRUE(WorldSaveService::upsert_container_records(path, {record}));
    const auto accepted = ReadFileBytes(path);
    std::vector<WorldSaveService::ContainerRecord> loaded;
    ASSERT_TRUE(WorldSaveService::read_container_records(path, loaded));
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded.front().payload, record.payload);
    record.payload += 'x';
    EXPECT_FALSE(WorldSaveService::upsert_container_records(path, {record}));
    EXPECT_EQ(ReadFileBytes(path), accepted);
    auto damaged = accepted;
    const auto oversized = static_cast<std::uint32_t>(SavedInputLimits::kRegionRecordBytes + 1);
    for (unsigned byte = 0; byte < 4; ++byte)
        damaged[18 + byte] = static_cast<char>((oversized >> (8u * byte)) & 255u);
    ASSERT_TRUE(WorldSaveService::write_file_durably(path, damaged));
    EXPECT_FALSE(WorldSaveService::read_container_records(path, loaded));
    // The shared central decoder also protects both chunk-only delegates.
    std::vector<std::shared_ptr<Chunk>> chunks;
    EXPECT_FALSE(WorldSaveService::read_region_chunks(path, chunks));
    EXPECT_FALSE(WorldSaveService::read_authoritative_region_chunks(path, 0, 31, 0, 31, chunks));
}

TEST(WorldSaveRegionFormat, RegionControlAndDeclaredAggregatesRefuseBeforePayloadReads) {
    using namespace Luminumbra::Persistence;
    TempSaveDir directory("region_control_limits");
    const auto path = directory.path / "container.lmr";
    std::vector<WorldSaveService::ContainerRecord> records;
    for (const std::string& bytes : {std::string("LMR1\2\0\1\0", 8),
                                     std::string("BAD!\2\0\0\0", 8),
                                     std::string("LMR1\2\0\0\0x", 9)}) {
        ASSERT_TRUE(WorldSaveService::write_file_durably(path, bytes));
        EXPECT_FALSE(WorldSaveService::read_container_records(path, records));
    }
    // Seventeen declared 64 MiB records exceed the 1 GiB read work budget.
    // No compressed payload is needed: all lengths are admitted before buffering.
    std::string bytes("LMR1\2\0\21\0", 8);
    for (unsigned index = 0; index < 17; ++index) {
        std::array<char, 18> header{};
        header[0] = static_cast<char>(index);
        header[8] = 1;
        header[13] = 4; // little-endian 64 MiB uncompressed size
        header[14] = 1;
        bytes.append(header.data(), header.size());
    }
    ASSERT_TRUE(WorldSaveService::write_file_durably(path, bytes));
    std::vector<std::string> errors;
    EXPECT_FALSE(WorldSaveService::read_container_records(path, records, &errors));
    ASSERT_FALSE(errors.empty());
    EXPECT_NE(errors.front().find("aggregate"), std::string::npos);
}

TEST(WorldSaveRegionFormat, RegionWriterAggregateAccepts512MiBAndRefusesOneMoreByte) {
    using namespace Luminumbra::Persistence;
    TempSaveDir directory("region_write_work_limits");
    const auto path = directory.path / "container.lmr";
    // Reuse one 64 MiB input for eight writes; compression keeps on-disk fixtures small.
    WorldSaveService::ContainerRecord record{
        .id = 0,
        .lod_level = 1,
        .flags = 0,
        .payload = std::string(SavedInputLimits::kRegionRecordBytes, 'x')};
    for (std::uint64_t index = 0; index < 8; ++index) {
        record.id = index;
        ASSERT_TRUE(WorldSaveService::upsert_container_records(path, {record}));
    }
    const auto accepted = ReadFileBytes(path);
    record.id = 8;
    record.payload = "x";
    EXPECT_FALSE(WorldSaveService::upsert_container_records(path, {record}));
    EXPECT_EQ(ReadFileBytes(path), accepted);
}

TEST(WorldSaveRegionFormat, RegionReadAggregateAcceptsExactlyOneGiBOfDeclaredWork) {
    using namespace Luminumbra::Persistence;
    TempSaveDir directory("region_read_work_limits");
    const auto path = directory.path / "container.lmr";
    WorldSaveService::ContainerRecord record{
        .id = 0,
        .lod_level = 1,
        .flags = 0,
        .payload = std::string(SavedInputLimits::kRegionRecordBytes, 'x')};
    ASSERT_TRUE(WorldSaveService::upsert_container_records(path, {record}));
    const auto single = ReadFileBytes(path);
    const auto compressed = single.substr(26);
    auto header = single.substr(8, 18);
    std::string exact("LMR1\2\0\20\0", 8);
    for (unsigned index = 0; index < 16; ++index) {
        header[0] = static_cast<char>(index);
        exact += header;
    }
    for (unsigned index = 0; index < 16; ++index)
        exact += compressed;
    ASSERT_TRUE(WorldSaveService::write_file_durably(path, exact));
    std::vector<std::shared_ptr<Chunk>> chunks;
    ASSERT_TRUE(WorldSaveService::read_region_chunks(path, chunks));
    EXPECT_TRUE(chunks.empty());
    // Insert a seventeenth one-byte declared record and one compressed byte.
    exact[6] = 17;
    std::string extra(18, '\0');
    extra[0] = 16;
    extra[8] = 1;
    extra[10] = 1;
    extra[14] = 1;
    exact.insert(8 + 16 * 18, extra);
    exact += 'x';
    ASSERT_TRUE(WorldSaveService::write_file_durably(path, exact));
    EXPECT_FALSE(WorldSaveService::read_region_chunks(path, chunks));
}

TEST(WorldSaveRegionFormat, RegionRecordCountAcceptsAll65535EntriesAndRefusesOneMore) {
    using namespace Luminumbra::Persistence;
    TempSaveDir directory("region_count_limits");
    const auto path = directory.path / "container.lmr";
    std::vector<WorldSaveService::ContainerRecord> records;
    records.reserve(SavedInputLimits::kRegionRecords + 1);
    for (std::size_t index = 0; index < SavedInputLimits::kRegionRecords; ++index)
        records.push_back({.id = index, .lod_level = 1, .flags = 0, .payload = "x"});
    ASSERT_TRUE(WorldSaveService::upsert_container_records(path, records));
    const auto accepted = ReadFileBytes(path);
    std::vector<WorldSaveService::ContainerRecord> loaded;
    ASSERT_TRUE(WorldSaveService::read_container_records(path, loaded));
    EXPECT_EQ(loaded.size(), SavedInputLimits::kRegionRecords);
    records.push_back(
        {.id = SavedInputLimits::kRegionRecords, .lod_level = 1, .flags = 0, .payload = "x"});
    EXPECT_FALSE(WorldSaveService::upsert_container_records(path, records));
    EXPECT_EQ(ReadFileBytes(path), accepted);
}

TEST(WorldSaveRegionFormat, AbsoluteRegionLengthAdmissionIsInclusiveAndDoesNotNarrowTo32Bits) {
    using Luminumbra::Persistence::SavedInputLimits::RegionFileLengthAllowed;
    // 2 * (8 + 65535 * (18 + compressBound(64 MiB))). A physically valid
    // region hits the aggregate work limit first; test its absolute length
    // admission without manufacturing a multi-terabyte file or allocating it.
    EXPECT_TRUE(RegionFileLengthAllowed(8'830'457'214'916ull));
    EXPECT_FALSE(RegionFileLengthAllowed(8'830'457'214'917ull));
    EXPECT_FALSE(RegionFileLengthAllowed(std::numeric_limits<std::uint64_t>::max()));
}

TEST(WorldSaveRegionFormat, PreservedCompressedRecordsObeyTheirWriterDerivedBound) {
    using namespace Luminumbra::Persistence;
    TempSaveDir directory("preserved_compression_bound");
    const auto path = directory.path / "container.lmr";
    for (const bool over : {false, true}) {
        // A one-byte raw input has compressBound(1) == 17. Chunk-only readers
        // preserve far records without decoding them; the writer still admits
        // their lengths before publishing a merge.
        std::string bytes("LMR1\2\0\1\0", 8);
        std::array<char, 18> header{};
        header[8] = 1;
        header[10] = 1;
        header[14] = over ? 18 : 17;
        bytes.append(header.data(), header.size());
        bytes.append(over ? 18 : 17, 'x');
        ASSERT_TRUE(WorldSaveService::write_file_durably(path, bytes));
        std::vector<std::shared_ptr<Chunk>> chunks;
        ASSERT_TRUE(WorldSaveService::read_region_chunks(path, chunks));
        EXPECT_EQ(WorldSaveService::upsert_container_records(path, {}), !over);
        EXPECT_EQ(ReadFileBytes(path), bytes);
    }
}

TEST(WorldSaveRegionFormat, CompressedRecordAndAggregateLengthsAreAdmittedBeforeMissingPayload) {
    using namespace Luminumbra::Persistence;
    TempSaveDir directory("compressed_lengths");
    const auto path = directory.path / "container.lmr";
    const auto append_record = [](std::string& bytes, unsigned id, std::uint32_t size) {
        std::array<char, 18> header{};
        header[0] = static_cast<char>(id);
        header[8] = 1;
        header[10] = 1;
        for (unsigned byte = 0; byte < 4; ++byte)
            header[14 + byte] = static_cast<char>((size >> (8u * byte)) & 255u);
        bytes.append(header.data(), header.size());
    };
    for (const bool aggregate : {false, true}) {
        for (const bool over : {false, true}) {
            std::string bytes("LMR1\2\0\0\0", 8);
            auto budget = aggregate ? SavedInputLimits::kRegionCompressedAggregateBytes
                                    : SavedInputLimits::kRegionCompressedBytes;
            budget += over ? 1 : 0;
            unsigned count = 0;
            while (budget != 0) {
                const auto size =
                    aggregate
                        ? std::min<std::uint64_t>(budget, SavedInputLimits::kRegionCompressedBytes)
                        : budget;
                append_record(bytes, count++, static_cast<std::uint32_t>(size));
                budget -= size;
            }
            bytes[6] = static_cast<char>(count);
            ASSERT_TRUE(WorldSaveService::write_file_durably(path, bytes));
            std::vector<std::shared_ptr<Chunk>> chunks;
            std::vector<std::string> errors;
            EXPECT_FALSE(WorldSaveService::read_region_chunks(path, chunks, &errors));
            ASSERT_FALSE(errors.empty());
            // At the exact cap length admission succeeds and reaches the later
            // missing-payload check. One byte over must refuse at the limit itself.
            EXPECT_NE(errors.front().find(over ? "compatibility limit" : "lengths do not match"),
                      std::string::npos);
        }
    }
}

TEST(WorldSaveRegionFormat, SavedJsonDepthBoundaryCoversMetadataManifestAndPreset) {
    using namespace Luminumbra::Persistence;
    TempSaveDir directory("json_depth");
    // Root object plus nested arrays. Brackets and escaped quotes inside a
    // string do not contribute to depth; both object and array roots count.
    const auto nested = [](std::size_t count) {
        return std::string(count, '[') + R"("brackets [ { \" ] }")" + std::string(count, ']');
    };
    for (const bool over : {false, true}) {
        const auto depth = SavedInputLimits::kJsonNestingDepth - 1 + (over ? 1u : 0u);
        const auto extension = nested(depth);
        const auto metadata = R"({"container_version":2,"extension":)" + extension + "}";
        const auto manifest =
            R"({"schema":"luminumbra.persistence.world_manifest.v1","container":"LMR1","container_version":2,"extension":)" +
            extension + "}";
        for (const auto& name : {"world_info.json", "manifest.json", "preset.json"}) {
            const auto path = directory.path / name;
            const bool is_metadata = std::string(name) == "world_info.json";
            ASSERT_TRUE(
                WorldSaveService::write_file_durably(path, is_metadata ? metadata : manifest));
            nlohmann::json loaded = {{"unchanged", true}};
            const auto original = loaded;
            bool missing = true;
            SavedJsonFailure failure = SavedJsonFailure::None;
            EXPECT_EQ(
                ReadSavedJson(
                    path, SavedInputLimits::kMetadataBytes, loaded, missing, is_metadata, &failure),
                !over);
            EXPECT_FALSE(missing);
            EXPECT_EQ(failure, over ? SavedJsonFailure::Limit : SavedJsonFailure::None);
            if (over) {
                EXPECT_EQ(loaded, original);
            }
        }
        // Exercise the real service consumers as well as their shared reader.
        for (const bool metadata_input : {false, true}) {
            const auto world =
                directory.path / (std::string(metadata_input ? "metadata-" : "manifest-") +
                                  (over ? "refused" : "accepted"));
            WorldStreamingState empty;
            ASSERT_TRUE(WorldSaveService{}.save_world(empty, world));
            const auto path = metadata_input ? world / "world_info.json"
                                             : WorldSaveService::world_manifest_path(world);
            ASSERT_TRUE(
                WorldSaveService::write_file_durably(path, metadata_input ? metadata : manifest));
            WorldStreamingState loaded;
            std::vector<std::string> errors;
            EXPECT_EQ(WorldSaveService{}.load_world(loaded, world, errors), !over);
            EXPECT_TRUE(loaded.empty());
            EXPECT_EQ(errors.empty(), !over);
            if (over) {
                ASSERT_EQ(errors.size(), 1u);
                EXPECT_NE(errors.front().find("Saved-input admission refused"), std::string::npos);
            }
        }
        // Legacy metadata publication requires an existing directory; admission
        // still refuses excessive depth before creating any file.
        const auto destination = directory.path / (over ? "refused" : "accepted");
        ASSERT_TRUE(std::filesystem::create_directory(destination));
        EXPECT_EQ(WorldSaveService::save_metadata(metadata, destination), !over);
        EXPECT_EQ(std::filesystem::exists(destination / "world_info.json"), !over);
        if (over) {
            EXPECT_TRUE(std::filesystem::is_empty(destination));
        }
    }
    EXPECT_TRUE(SavedInputLimits::JsonDepthAllowed(nested(SavedInputLimits::kJsonNestingDepth)));
    EXPECT_FALSE(
        SavedInputLimits::JsonDepthAllowed(nested(SavedInputLimits::kJsonNestingDepth + 1)));
}

TEST(WorldSaveRegionFormat, JsonValueDepthAccountsForBinaryEncodingAndRejectsDiscardedValues) {
    using namespace Luminumbra::Persistence;
    const auto binary = nlohmann::json::binary({1});
    EXPECT_FALSE(AdmitJsonValueDepth(binary, 1));
    EXPECT_FALSE(AdmitJsonValueDepth(binary, 2, 3));
    EXPECT_TRUE(AdmitJsonValueDepth(binary, 2, 4));
    const auto object = nlohmann::json{{"payload", binary}};
    EXPECT_FALSE(AdmitJsonValueDepth(object, 2));
    EXPECT_FALSE(AdmitJsonValueDepth(object, 3, 4));
    EXPECT_TRUE(AdmitJsonValueDepth(object, 3, 5));
    auto nested = binary;
    for (std::size_t depth = 0; depth < SavedInputLimits::kJsonNestingDepth - 2; ++depth)
        nested = nlohmann::json::array({std::move(nested)});
    std::string bytes;
    ASSERT_TRUE(BoundedJsonDump(nested, SavedInputLimits::kMetadataBytes, bytes));
    EXPECT_TRUE(SavedInputLimits::JsonDepthAllowed(bytes));
    const auto accepted = bytes;
    nested = nlohmann::json::array({std::move(nested)});
    EXPECT_FALSE(BoundedJsonDump(nested, SavedInputLimits::kMetadataBytes, bytes));
    EXPECT_EQ(bytes, accepted);
    const auto discarded = nlohmann::json(nlohmann::json::value_t::discarded);
    for (const auto& invalid : {discarded, nlohmann::json::array({discarded})}) {
        EXPECT_FALSE(BoundedJsonDump(invalid, SavedInputLimits::kMetadataBytes, bytes));
        EXPECT_EQ(bytes, accepted);
    }
}

TEST(WorldSaveRegionFormat, PreparedWorldWriteReusesRecordsAndInvalidPreparationCannotPublish) {
    TempSaveDir directory("prepared_records");
    WorldStreamingState state;
    auto chunk = state.get_or_create_chunk(IVec3(0, 0, 0));
    chunk->heightmap_data = {8.0f};
    WorldSaveService::PreparedWorldWrite prepared;
    ASSERT_TRUE(prepared.prepare(state, directory.path, false));
    EXPECT_FALSE(std::filesystem::exists(WorldSaveService::world_manifest_path(directory.path)));
    chunk->heightmap_data = {19.0f};
    ASSERT_TRUE(prepared.publish());
    EXPECT_FALSE(prepared.publish());
    WorldStreamingState loaded;
    std::vector<std::string> errors;
    ASSERT_TRUE(WorldSaveService{}.load_world(loaded, directory.path, errors));
    ASSERT_NE(loaded.find_chunk(chunk->get_id()), nullptr);
    EXPECT_EQ(loaded.find_chunk(chunk->get_id())->heightmap_data, std::vector<float>{8.0f});
    const auto path = WorldSaveService::region_file_path(directory.path, 0, 0);
    const auto original = ReadFileBytes(path);
    chunk->mark_voxel_data_dirty();
    WorldSaveService::PreparedWorldWrite empty;
    EXPECT_FALSE(empty.publish());
    const auto invalid = directory.path / "invalid";
    ASSERT_TRUE(WorldSaveService::write_file_durably(invalid / "world_info.json", "{"));
    EXPECT_FALSE(prepared.prepare(state, invalid, true, &errors));
    EXPECT_FALSE(prepared.publish(&errors));
    EXPECT_EQ(ReadFileBytes(path), original);
    ASSERT_TRUE(prepared.prepare(state, directory.path, false));
    WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    EXPECT_FALSE(prepared.publish(&errors));
    WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
    EXPECT_FALSE(prepared.publish(&errors));
    EXPECT_EQ(ReadFileBytes(path), original);
    EXPECT_TRUE(chunk->is_voxel_data_dirty());
}

TEST(WorldSaveRegionFormat, ChunkSnapshotDepthAdmissionPrecedesParsing) {
    using namespace Luminumbra::Persistence;
    WorldStreamingState state;
    state.get_or_create_chunk(IVec3(0, 0, 0));
    const auto canonical = SerializeWorldStreamingStateSnapshotJson(state);
    for (const bool over : {false, true}) {
        auto snapshot = nlohmann::json::parse(canonical);
        nlohmann::json extension = 0;
        for (std::size_t depth = 1; depth < SavedInputLimits::kJsonNestingDepth + (over ? 1u : 0u);
             ++depth)
            extension = nlohmann::json::array({std::move(extension)});
        snapshot["extension"] = std::move(extension);
        WorldStreamingState loaded;
        loaded.get_or_create_chunk(IVec3(1, 0, 0));
        std::vector<std::string> errors;
        EXPECT_EQ(LoadWorldStreamingStateSnapshotJson(snapshot.dump(), loaded, errors), !over);
        EXPECT_EQ(loaded.size(), over ? 0u : 1u);
        EXPECT_EQ(errors.empty(), !over);
        auto chunk = snapshot.at("chunks").front();
        chunk["extension"] = snapshot.at("extension");
        errors.clear();
        EXPECT_EQ(ValidateWorldStreamingChunkFormatJson(chunk.dump(), errors), !over);
        EXPECT_EQ(errors.empty(), !over);
    }
}

TEST(WorldSaveRegionFormat, LedgerReadsRefuseWrongTypesAndPartialDataWithoutChangingOutput) {
    using namespace Luminumbra;
    TempSaveDir directory("ledger_input");
    const auto path = WorldSaveService::active_regions_path(directory.path);
    world::ActiveRegionLedger ledger;
    ledger.set_local_anchor(Vec3(8, 18, 8));
    const auto original = ledger.canonical_bytes();
    std::filesystem::create_directories(path);
    EXPECT_FALSE(WorldSaveService::load_active_regions(ledger, directory.path));
    EXPECT_EQ(ledger.canonical_bytes(), original);
    std::filesystem::remove(path);
    for (const auto& bytes : {std::string{}, std::string("partial")}) {
        ASSERT_TRUE(WorldSaveService::write_file_durably(path, bytes));
        EXPECT_FALSE(WorldSaveService::load_active_regions(ledger, directory.path));
        EXPECT_EQ(ledger.canonical_bytes(), original);
    }
    std::filesystem::remove(path);
    ASSERT_TRUE(WorldSaveService::load_active_regions(ledger, directory.path));
    EXPECT_TRUE(ledger.records().empty());
    EXPECT_FALSE(ledger.local_anchor().has_value());
#if !defined(_WIN32)
    std::filesystem::create_symlink(directory.path / "absent", path);
    EXPECT_FALSE(WorldSaveService::load_active_regions(ledger, directory.path));
    EXPECT_FALSE(ledger.local_anchor().has_value());
#endif
}
