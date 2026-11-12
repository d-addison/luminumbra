//  persistence v2: LMR1 region container round-trip, obsolete-format refusal
// (world-hash equality is the migration gate),.bak retention, and
// O(edited regions) incremental saves. Container spec:
// design decisions, section 3.
#include "gtest/gtest.h"

#include "../support/PersistenceFileOps.h"
#include "../support/UniqueTempPath.h"
#include "persistence/SaveTransaction.h"
#include "persistence/SavedInputFile.h"

#include "ecs/EntitySnapshot.h"
#include "persistence/SavedJsonAdmission.h"
#include "persistence/WorldPersistenceRoundtrip.h"
#include "persistence/WorldSaveEncoding.h"
#include "persistence/WorldSaveService.h"
#include "world/ActiveRegionLedger.h"
#include "world/WorldClock.h"
#include "world/WorldStreamingState.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_set>
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

class PauseSavedRead {
public:
    explicit PauseSavedRead(const std::filesystem::path& path)
        : m_path(path) {
        Active() = this;
        Luminumbra::Persistence::detail::set_file_read_observer_for_testing(&Observe);
    }
    ~PauseSavedRead() {
        Luminumbra::Persistence::detail::set_file_read_observer_for_testing(nullptr);
        Active() = nullptr;
    }
    PauseSavedRead(const PauseSavedRead&) = delete;
    PauseSavedRead& operator=(const PauseSavedRead&) = delete;

    std::latch opened{1};
    std::latch release{1};

private:
    static PauseSavedRead*& Active() {
        static PauseSavedRead* active = nullptr;
        return active;
    }
    static void Observe(const std::filesystem::path& path, bool opening) {
        auto* active = Active();
        if (active && opening && path == active->m_path && active->m_first.exchange(false)) {
            active->opened.count_down();
            active->release.wait();
        }
    }
    std::filesystem::path m_path;
    std::atomic<bool> m_first{true};
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

// Keep every chunk in region (0, 0); the encoder threshold applies per region.
void PopulateEncodingWorld(WorldStreamingState& state, int count, int offset_x = 0) {
    for (int i = 0; i < count; ++i) {
        auto chunk =
            AddFixtureChunk(state, {offset_x + i % 16, 0, i / 16}, static_cast<unsigned>(i));
        constexpr std::size_t kLattice = static_cast<std::size_t>(Luminumbra::CHUNK_SIZE_X + 1) *
                                         (Luminumbra::CHUNK_SIZE_Y + 1) *
                                         (Luminumbra::CHUNK_SIZE_Z + 1);
        chunk->sdf_data.resize(kLattice, 1.0f);
        chunk->water_level_data.resize(16, 1.0f);
        chunk->water_flow_data.resize(16, Vec2(0.125f, -0.25f));
        chunk->water_sim_terrain_height.resize(16, 0.5f);
        chunk->water_depth_mm.assign(16, 1000);
        chunk->water_bed_mm.assign(16, 500);
        chunk->water_edge_flux.assign(32, 0);
        chunk->current_water_resolution.store(4);
        chunk->mark_sdf_generated_current_params();
        if (i % 3 == 0) {
            chunk->mark_voxel_data_dirty();
        } else if (i % 3 == 1) {
            chunk->set_water_authority();
            chunk->mark_water_data_dirty();
        }
    }
}

class ChunkEncodingControl {
public:
    explicit ChunkEncodingControl(unsigned workers, bool rendezvous = false)
        : m_rendezvous(rendezvous) {
        Active() = this;
        Luminumbra::Persistence::detail::set_chunk_encode_workers_for_testing(workers);
        Luminumbra::Persistence::detail::set_chunk_encode_hook_for_testing(&Observe);
    }
    ~ChunkEncodingControl() {
        Luminumbra::Persistence::detail::set_chunk_encode_hook_for_testing(nullptr);
        Luminumbra::Persistence::detail::set_chunk_encode_workers_for_testing(0);
        Active() = nullptr;
    }
    ChunkEncodingControl(const ChunkEncodingControl&) = delete;
    ChunkEncodingControl& operator=(const ChunkEncodingControl&) = delete;

    // Read only after all encoders have joined.
    unsigned maximum = 0;
    unsigned calls = 0;
    bool caller_only = true;
    std::function<void(std::size_t)> before_encode;

private:
    static ChunkEncodingControl*& Active() {
        static ChunkEncodingControl* active = nullptr;
        return active;
    }
    static void Observe(std::size_t index) {
        auto& control = *Active();
        {
            std::unique_lock lock(control.m_mutex);
            ++control.calls;
            ++control.m_active;
            control.maximum = std::max(control.maximum, control.m_active);
            control.caller_only =
                control.caller_only && std::this_thread::get_id() == control.m_caller;
            control.m_entered.notify_all();
            if (control.m_rendezvous && control.calls == 1) {
                // A serial regression must fail the overlap assertion instead of hanging.
                control.m_entered.wait_for(
                    lock, std::chrono::seconds(5), [&control] { return control.maximum > 1; });
            }
            --control.m_active;
        }
        if (control.before_encode)
            control.before_encode(index);
    }
    const bool m_rendezvous;
    const std::thread::id m_caller = std::this_thread::get_id();
    std::mutex m_mutex;
    std::condition_variable m_entered;
    unsigned m_active = 0;
};

TEST(WorldSaveRegionFormat, ChunkEncodingFansOutOnlyAtTheEncodingThreshold) {
    for (const int count : {0, 1, 127, 128, 129}) {
        SCOPED_TRACE(count);
        TempSaveDir directory("encode_concurrency");
        WorldStreamingState state;
        PopulateEncodingWorld(state, count);
        ChunkEncodingControl control(4, count >= 128);
        WorldSaveService service;
        std::vector<std::string> errors;
        ASSERT_TRUE(service.save_world(state, directory.path, &errors));
        ASSERT_TRUE(errors.empty());
        EXPECT_EQ(control.calls, static_cast<unsigned>(count));
        if (count >= 128) {
            EXPECT_GT(control.maximum, 1u);
            EXPECT_LE(control.maximum, 4u);
            EXPECT_FALSE(control.caller_only);
        } else {
            EXPECT_EQ(control.maximum, count == 0 ? 0u : 1u);
            EXPECT_TRUE(control.caller_only);
        }
    }
}

TEST(WorldSaveRegionFormat, ChunkEncodingWorkerCountsProduceIdenticalRegionAndManifestBytes) {
    TempSaveDir serial("encode_serial"), parallel("encode_parallel");
    WorldStreamingState state;
    PopulateEncodingWorld(state, 129);
    WorldSaveService service;
    for (const unsigned workers : {1u, 4u}) {
        SCOPED_TRACE(workers);
        const auto& dir = workers == 1 ? serial.path : parallel.path;
        ChunkEncodingControl control(workers, workers > 1);
        std::vector<std::string> errors;
        ASSERT_TRUE(service.save_world(state, dir, &errors));
        ASSERT_TRUE(errors.empty());
        EXPECT_EQ(control.calls, 129u);
        if (workers == 1) {
            EXPECT_EQ(control.maximum, 1u);
            EXPECT_TRUE(control.caller_only);
        } else {
            EXPECT_GT(control.maximum, 1u);
        }
    }
    const auto serial_region = WorldSaveService::region_file_path(serial.path, 0, 0);
    const auto parallel_region = WorldSaveService::region_file_path(parallel.path, 0, 0);
    const auto original = ReadFileBytes(serial_region);
    ASSERT_FALSE(original.empty());
    EXPECT_EQ(original, ReadFileBytes(parallel_region));
    EXPECT_EQ(ReadFileBytes(WorldSaveService::world_manifest_path(serial.path)),
              ReadFileBytes(WorldSaveService::world_manifest_path(parallel.path)));

    // A large resident region with cached records needs no encoders.
    {
        ChunkEncodingControl control(4);
        ASSERT_TRUE(service.save_world(state, parallel.path));
        EXPECT_EQ(control.calls, 0u);
        EXPECT_EQ(ReadFileBytes(parallel_region), original);
    }
    // Only the changed record needs encoding, so it stays on the caller thread.
    state.find_chunk(IVec3(0, 0, 0))->heightmap_data[0] += 1.0f;
    for (const unsigned workers : {1u, 4u}) {
        const auto& dir = workers == 1 ? serial.path : parallel.path;
        ChunkEncodingControl control(workers);
        ASSERT_TRUE(service.save_world(state, dir));
        EXPECT_EQ(control.calls, 1u);
        EXPECT_EQ(control.maximum, 1u);
        EXPECT_TRUE(control.caller_only);
    }
    EXPECT_NE(ReadFileBytes(serial_region), original);
    EXPECT_EQ(ReadFileBytes(serial_region), ReadFileBytes(parallel_region));

    // Replace many existing records, preserve one absent resident, and insert one new record.
    ASSERT_TRUE(state.erase_chunk(IVec3(0, 0, 0)));
    for (const auto& chunk : state.snapshot_chunks())
        chunk->heightmap_data[1] += 0.25f;
    state.get_or_create_chunk({16, 0, 16})->mark_sdf_generated_current_params();
    for (const unsigned workers : {1u, 4u}) {
        const auto& dir = workers == 1 ? serial.path : parallel.path;
        ChunkEncodingControl control(workers, workers > 1);
        ASSERT_TRUE(service.save_world(state, dir));
        EXPECT_EQ(control.calls, 129u);
        if (workers > 1) {
            EXPECT_GT(control.maximum, 1u);
        }
    }
    EXPECT_EQ(ReadFileBytes(serial_region), ReadFileBytes(parallel_region));
    EXPECT_EQ(ReadFileBytes(WorldSaveService::world_manifest_path(serial.path)),
              ReadFileBytes(WorldSaveService::world_manifest_path(parallel.path)));
    std::vector<std::shared_ptr<Chunk>> restored;
    ASSERT_TRUE(WorldSaveService::read_region_chunks(parallel_region, restored));
    EXPECT_EQ(restored.size(), 130u);
    EXPECT_TRUE(std::any_of(restored.begin(), restored.end(), [](const auto& chunk) {
        return chunk->get_coords() == IVec3(0, 0, 0);
    }));
}

TEST(WorldSaveRegionFormat, ChunkEncodingKeepsIndependentSmallRegionsOnTheCaller) {
    WorldStreamingState state;
    PopulateEncodingWorld(state, 64);
    PopulateEncodingWorld(state, 64, 32);
    TempSaveDir directory("encode_small_regions");
    ChunkEncodingControl control(4);
    WorldSaveService service;
    ASSERT_TRUE(service.save_world(state, directory.path));
    EXPECT_EQ(control.calls, 128u);
    EXPECT_EQ(control.maximum, 1u);
    EXPECT_TRUE(control.caller_only);
}

TEST(WorldSaveRegionFormat, ChunkEncodingCompressionFailureKeepsErrorsAndLeavesNoRegion) {
    WorldStreamingState state;
    PopulateEncodingWorld(state, 129);
    // Base64 of 48 MiB occupies exactly 64 MiB; JSON makes the payload exceed the limit.
    state.snapshot_chunks().front()->material_data.assign(48u * 1024u * 1024u, 7u);
    WorldSaveService service;
    for (const unsigned workers : {1u, 4u}) {
        SCOPED_TRACE(workers);
        TempSaveDir directory("encode_compression_failure");
        ChunkEncodingControl control(workers);
        // A later worker exception must not replace the first item's compression diagnostic.
        control.before_encode = [](std::size_t index) {
            if (index == 7)
                throw std::runtime_error("later encoding failure");
        };
        std::vector<std::string> errors{"earlier diagnostic"};
        EXPECT_FALSE(service.save_world(state, directory.path, &errors));
        EXPECT_EQ(errors,
                  (std::vector<std::string>{
                      "earlier diagnostic",
                      "region record payload exceeds the 64 MiB compatibility limit"}));
        EXPECT_FALSE(std::filesystem::exists(WorldSaveService::region_directory(directory.path)));
        EXPECT_FALSE(
            std::filesystem::exists(WorldSaveService::world_manifest_path(directory.path)));
    }
}

TEST(WorldSaveRegionFormat, ChunkRecordEncodingRetainsCompactJsonDumpBytes) {
    namespace P = Luminumbra::Persistence;
    WorldStreamingState state;
    PopulateEncodingWorld(state, 3);
    for (const auto& chunk : state.snapshot_chunks()) {
        const auto payload = P::SerializeChunkRecordPayloadJson(*chunk);
        EXPECT_EQ(payload, nlohmann::json::parse(payload).dump());
        chunk->max_water_delta_last_tick = std::numeric_limits<float>::infinity();
        const auto non_finite = P::SerializeChunkRecordPayloadJson(*chunk);
        EXPECT_EQ(non_finite, nlohmann::json::parse(non_finite).dump());
        EXPECT_NE(non_finite.find(":null"), std::string::npos);
    }
}

TEST(WorldSaveRegionFormat, ChunkEncodingInFlightLimitSerializesWithoutChangingBytes) {
    TempSaveDir unlimited("encode_unlimited"), limited("encode_limited");
    WorldStreamingState state;
    PopulateEncodingWorld(state, 129);
    WorldSaveService service;
    {
        ChunkEncodingControl control(4);
        std::vector<std::string> errors;
        ASSERT_TRUE(service.save_world(state, unlimited.path, &errors));
        ASSERT_TRUE(errors.empty());
    }
    std::atomic<unsigned> active{0};
    std::atomic<unsigned> peak{0};
    {
        ChunkEncodingControl control(4);
        // One byte admits a single record at a time, and an oversized record still proceeds.
        Luminumbra::Persistence::detail::set_chunk_encode_in_flight_limit_for_testing(1);
        control.before_encode = [&](std::size_t) {
            const unsigned now = ++active;
            unsigned seen = peak.load();
            while (now > seen && !peak.compare_exchange_weak(seen, now)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            --active;
        };
        std::vector<std::string> errors;
        const bool saved = service.save_world(state, limited.path, &errors);
        Luminumbra::Persistence::detail::set_chunk_encode_in_flight_limit_for_testing(0);
        ASSERT_TRUE(saved);
        ASSERT_TRUE(errors.empty());
        EXPECT_EQ(control.calls, 129u);
    }
    EXPECT_EQ(peak.load(), 1u);
    const auto expected = ReadFileBytes(WorldSaveService::region_file_path(unlimited.path, 0, 0));
    ASSERT_FALSE(expected.empty());
    EXPECT_EQ(expected, ReadFileBytes(WorldSaveService::region_file_path(limited.path, 0, 0)));
}

TEST(WorldSaveRegionFormat, SnapshotInputByteCountMatchesTheCapturedBytes) {
    namespace P = Luminumbra::Persistence;
    WorldStreamingState state;
    PopulateEncodingWorld(state, 3);
    const auto sparse = AddFixtureChunk(state, IVec3(40, 1, 5), 9u);
    for (const auto& chunk : state.snapshot_chunks()) {
        SCOPED_TRACE(chunk->get_id());
        EXPECT_EQ(P::ChunkSnapshotInputByteCount(*chunk),
                  P::ChunkSnapshotInputBytes(*chunk).size());
    }
    EXPECT_EQ(P::ChunkSnapshotInputByteCount(*sparse), P::ChunkSnapshotInputBytes(*sparse).size());
}

TEST(WorldSaveRegionFormat, GuardedJsonDumpKeepsNonAsciiTextAsUtf8) {
    namespace P = Luminumbra::Persistence;
    const nlohmann::json value = {{"name", "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x8C\x8D"},
                                  {"list", {"\xC3\xBC", "ascii"}}};
    EXPECT_EQ(P::detail::dump_json_guarded(value), value.dump());
    EXPECT_EQ(P::detail::dump_json_guarded(value, 4), value.dump(4));
    EXPECT_NE(P::detail::dump_json_guarded(value).find("caf\xC3\xA9"), std::string::npos);
}

TEST(WorldSaveRegionFormat, GuardedJsonDumpMatchesTheLibraryDump) {
    namespace P = Luminumbra::Persistence;
    const nlohmann::json value = {{"name", "quote\" and \\ slash"},
                                  {"values", {1, 2.5, -3}},
                                  {"nested", {{"flag", true}, {"none", nullptr}}}};
    EXPECT_EQ(P::detail::dump_json_guarded(value), value.dump());
    EXPECT_EQ(P::detail::dump_json_guarded(value, 4), value.dump(4));
    EXPECT_EQ(P::detail::dump_json_guarded(value, 0), value.dump(0));
}

TEST(WorldSaveRegionFormat, ChunkEncodingRethrowsFirstWorkerExceptionInInputOrder) {
    WorldStreamingState state;
    PopulateEncodingWorld(state, 129);
    WorldSaveService service;
    for (const unsigned workers : {1u, 4u}) {
        SCOPED_TRACE(workers);
        TempSaveDir directory("encode_exception");
        ChunkEncodingControl control(workers);
        std::mutex error_mutex;
        std::condition_variable later_error;
        bool later_failed = false;
        control.before_encode = [&](std::size_t index) {
            if (index == 3 && workers > 1) {
                std::unique_lock lock(error_mutex);
                later_error.wait_for(lock, std::chrono::seconds(5), [&] { return later_failed; });
            }
            if (index == 7) {
                const std::lock_guard lock(error_mutex);
                later_failed = true;
                later_error.notify_all();
            }
            if (index == 3 || index == 7)
                throw std::runtime_error("encoder[" + std::to_string(index) + "] failed");
        };
        std::vector<std::string> errors;
        EXPECT_FALSE(service.save_world(state, directory.path, &errors));
        EXPECT_EQ(
            errors,
            (std::vector<std::string>{"failed to prepare world region files: encoder[3] failed"}));
        if (workers > 1) {
            EXPECT_TRUE(later_failed);
        }
        EXPECT_FALSE(std::filesystem::exists(WorldSaveService::region_directory(directory.path)));
        EXPECT_FALSE(
            std::filesystem::exists(WorldSaveService::world_manifest_path(directory.path)));
        // The next save also proves workers have joined and the previous failure released
        // ownership.
        control.before_encode = {};
        errors.clear();
        EXPECT_TRUE(service.save_world(state, directory.path, &errors));
        EXPECT_TRUE(errors.empty());
    }
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

TEST(WorldSaveRegionFormat, ChunkReadBatchVerifiesEachRegionOnceUntilAPublication) {
    namespace P = Luminumbra::Persistence;
    TempSaveDir save("batched_exact_reads");
    WorldStreamingState state;
    for (int x = 0; x != 2; ++x) {
        const auto chunk = state.get_or_create_chunk({x, 0, 0});
        chunk->set_state(ChunkState::Idle);
        chunk->sdf_data.assign(std::size_t{17} * 17 * 17, -1.0f - static_cast<float>(x));
        chunk->mark_voxel_data_dirty();
    }
    {
        P::SaveTransaction baseline(save.path, 1, {}, &WorldSaveService::member_digest);
        ASSERT_TRUE(baseline.begin(nullptr));
        ASSERT_TRUE(WorldSaveService{}.save_world(state, save.path));
        ASSERT_TRUE(baseline.commit(nullptr));
    }
    static std::filesystem::path counted;
    static int region_opens = 0;
    counted = WorldSaveService::region_file_path(save.path, 0, 0);
    struct StopCounting {
        ~StopCounting() {
            P::detail::set_file_read_observer_for_testing(nullptr);
        }
    } const stop_counting;
    P::detail::set_file_read_observer_for_testing(
        +[](const std::filesystem::path& path, bool opening) {
            if (opening && path == counted)
                ++region_opens;
        });
    const auto read_front = [&save](int x, float expected) {
        const auto opens = region_opens;
        const auto result = WorldSaveService::read_chunk(save.path, {x, 0, 0}, true);
        EXPECT_EQ(result.status, WorldSaveService::ChunkReadStatus::Found);
        if (result.chunk && !result.chunk->sdf_data.empty()) {
            EXPECT_EQ(result.chunk->sdf_data.front(), expected);
        }
        return region_opens - opens;
    };
    {
        const WorldSaveService::ChunkReadBatch batch;
        EXPECT_EQ(read_front(0, -1.0f), 1);
        const auto full_reads = WorldSaveService::region_read_count_for_testing();
        // Cache hits still open a fresh handle for exact manifest and record comparison.
        EXPECT_EQ(read_front(1, -2.0f), 1);
        {
            const WorldSaveService::ChunkReadBatch nested;
            EXPECT_EQ(read_front(0, -1.0f), 1);
        }
        EXPECT_EQ(read_front(1, -2.0f), 1);
        EXPECT_EQ(WorldSaveService::region_read_count_for_testing(), full_reads);
        // A commit on this thread replaces verified members; the next read re-admits them.
        const auto first = state.get_or_create_chunk({0, 0, 0});
        first->sdf_data.front() = -9.0f;
        first->mark_voxel_data_dirty();
        {
            P::SaveTransaction transaction(save.path, 2, {}, &WorldSaveService::member_digest);
            ASSERT_TRUE(transaction.begin(nullptr));
            ASSERT_EQ(WorldSaveService{}.save_dirty_chunks(state, save.path).regions_written, 1u);
            ASSERT_TRUE(transaction.commit(nullptr));
        }
        EXPECT_EQ(read_front(0, -9.0f), 1);
        EXPECT_EQ(read_front(1, -2.0f), 1);
    }
    std::ofstream(counted, std::ios::binary | std::ios::trunc) << "invalid";
    EXPECT_EQ(WorldSaveService::read_chunk(save.path, {0, 0, 0}, true).status,
              WorldSaveService::ChunkReadStatus::Error);
    const WorldSaveService::ChunkReadBatch tampered;
    for (int x = 0; x != 2; ++x) {
        const auto opens = region_opens;
        EXPECT_EQ(WorldSaveService::read_chunk(save.path, {x, 0, 0}, true).status,
                  WorldSaveService::ChunkReadStatus::Error);
        EXPECT_EQ(region_opens - opens, 1);
    }
}

TEST(WorldSaveRegionFormat, RegionFingerprintIsReusedOnlyForIdenticalLod0Bytes) {
    namespace P = Luminumbra::Persistence;
    TempSaveDir save("fingerprint_reuse");
    WorldStreamingState state;
    for (int x = 0; x != 2; ++x) {
        const auto chunk = state.get_or_create_chunk({x, 0, 0});
        chunk->set_state(ChunkState::Idle);
        chunk->sdf_data.assign(std::size_t{17} * 17 * 17, -1.0f - static_cast<float>(x));
        chunk->mark_voxel_data_dirty();
    }
    {
        P::SaveTransaction baseline(save.path, 1, {}, &WorldSaveService::member_digest);
        ASSERT_TRUE(baseline.begin(nullptr));
        ASSERT_TRUE(WorldSaveService{}.save_world(state, save.path));
        ASSERT_TRUE(baseline.commit(nullptr));
    }
    const auto region = WorldSaveService::region_file_path(save.path, 0, 0);
    const auto read_every_path = [&save, &region]() {
        std::vector<std::shared_ptr<Chunk>> chunks;
        std::vector<WorldSaveService::ContainerRecord> records;
        return WorldSaveService::read_chunk(save.path, {0, 0, 0}, true).status ==
                   WorldSaveService::ChunkReadStatus::Found &&
               WorldSaveService::read_chunk(save.path, {1, 0, 0}, true).status ==
                   WorldSaveService::ChunkReadStatus::Found &&
               WorldSaveService::read_container_records(region, records) &&
               WorldSaveService::read_region_chunks(region, chunks) &&
               WorldSaveService::read_authoritative_region_chunks(region, 0, 31, 0, 31, chunks);
    };
    const auto first = WorldSaveService::region_fingerprint_count_for_testing();
    ASSERT_TRUE(read_every_path());
    // The commit may have hashed these bytes under another member path; the published path
    // hashes at most once, then every entry point reuses it.
    const auto hashed = WorldSaveService::region_fingerprint_count_for_testing();
    EXPECT_LE(hashed - first, 1u);
    for (int repeat = 0; repeat != 3; ++repeat) {
        ASSERT_TRUE(read_every_path());
    }
    EXPECT_EQ(WorldSaveService::region_fingerprint_count_for_testing(), hashed);

    // Flip one lod-0 payload byte in place: same path, identity and length.
    const auto bytes = ReadFileBytes(region);
    ASSERT_GE(bytes.size(), 8u);
    const auto* control = reinterpret_cast<const unsigned char*>(bytes.data());
    const std::size_t count = control[6] | (std::size_t{control[7]} << 8);
    std::size_t payload_offset = 8 + count * 18;
    std::size_t flipped = bytes.size();
    for (std::size_t index = 0; index != count; ++index) {
        const auto* entry = control + 8 + index * 18;
        if (entry[8] == 0) {
            flipped = payload_offset;
            break;
        }
        payload_offset += std::size_t{entry[14]} | (std::size_t{entry[15]} << 8) |
                          (std::size_t{entry[16]} << 16) | (std::size_t{entry[17]} << 24);
    }
    ASSERT_LT(flipped, bytes.size());
    const auto write_byte_in_place = [&region](std::size_t offset, char value) {
        std::fstream file(region, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(static_cast<std::streamoff>(offset));
        file.put(value);
        return static_cast<bool>(file);
    };
    ASSERT_TRUE(write_byte_in_place(flipped, static_cast<char>(bytes[flipped] ^ 0x5a)));
    EXPECT_EQ(WorldSaveService::read_chunk(save.path, {0, 0, 0}, true).status,
              WorldSaveService::ChunkReadStatus::Error);
    std::vector<WorldSaveService::ContainerRecord> tampered;
    EXPECT_FALSE(WorldSaveService::read_container_records(region, tampered));
    // The tampered bytes are hashed once; reusing that digest still fails the index comparison.
    EXPECT_EQ(WorldSaveService::region_fingerprint_count_for_testing(), hashed + 1);

    // Restored bytes match the index again; they are hashed once more and then reused.
    ASSERT_TRUE(write_byte_in_place(flipped, bytes[flipped]));
    ASSERT_EQ(ReadFileBytes(region), bytes);
    ASSERT_TRUE(read_every_path());
    ASSERT_TRUE(read_every_path());
    EXPECT_EQ(WorldSaveService::region_fingerprint_count_for_testing(), hashed + 2);
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
    for (const auto size : {4u, 17u * 17u * 17u - 1u, 17u * 17u * 17u + 1u}) {
        SCOPED_TRACE(size);
        chunk->sdf_data.resize(size);
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
}

TEST(WorldSaveRegionFormat, IndexedReadersWaitForTheCompleteCommit) {
    namespace P = Luminumbra::Persistence;
    for (int reader_kind = 0; reader_kind != 4; ++reader_kind) {
        SCOPED_TRACE(reader_kind);
        TempSaveDir save("indexed_read_publication");
        WorldStreamingState state;
        const auto chunk = state.get_or_create_chunk({0, 0, 0});
        chunk->set_state(ChunkState::Idle);
        chunk->sdf_data.assign(17u * 17u * 17u, -1.0f);
        chunk->mark_voxel_data_dirty();
        {
            P::SaveTransaction baseline(save.path, 1, {}, &WorldSaveService::member_digest);
            ASSERT_TRUE(baseline.begin(nullptr));
            ASSERT_TRUE(WorldSaveService{}.save_world(state, save.path));
            ASSERT_TRUE(baseline.commit(nullptr));
        }
        std::optional<WorldSaveService::SaveOwnership> ownership(std::in_place);
        P::SaveTransaction transaction(save.path, 2, {}, &WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(nullptr));
        chunk->sdf_data.front() = -9.0f;
        chunk->mark_voxel_data_dirty();
        ASSERT_EQ(WorldSaveService{}.save_dirty_chunks(state, save.path).regions_written, 1u);
        // Region bytes are new; the index is still the old commit. Each public reader must
        // wait for ownership rather than treating this internal state as corrupted authority.
        std::atomic<bool> finished{false};
        bool read_ok = false;
        std::vector<std::shared_ptr<Chunk>> chunks;
        const auto region = WorldSaveService::region_file_path(save.path, 0, 0);
        std::jthread reader([&] {
            if (reader_kind == 0) {
                const auto result = WorldSaveService::read_chunk(save.path, {0, 0, 0}, true);
                read_ok = result.status == WorldSaveService::ChunkReadStatus::Found;
                if (result.chunk)
                    chunks.push_back(result.chunk);
            } else if (reader_kind == 1) {
                read_ok = WorldSaveService::read_region_chunks(region, chunks);
            } else if (reader_kind == 2) {
                read_ok = WorldSaveService::read_authoritative_region_chunks(
                    region, 0, 31, 0, 31, chunks);
            } else {
                std::vector<WorldSaveService::ContainerRecord> records;
                read_ok = WorldSaveService::read_container_records(region, records) &&
                          records.size() == 1u && records.front().id == chunk->get_id();
            }
            finished.store(true, std::memory_order_release);
        });
        while (WorldSaveService::waiting_save_writers() == 0 &&
               !finished.load(std::memory_order_acquire))
            std::this_thread::yield();
        EXPECT_FALSE(finished.load(std::memory_order_acquire));
        EXPECT_EQ(WorldSaveService::waiting_save_writers(), 1u);
        EXPECT_TRUE(transaction.commit(nullptr));
        ownership.reset();
        reader.join();
        EXPECT_TRUE(read_ok);
        if (reader_kind != 3) {
            ASSERT_EQ(chunks.size(), 1u);
            ASSERT_FALSE(chunks.front()->sdf_data.empty());
            EXPECT_EQ(chunks.front()->sdf_data.front(), -9.0f);
        }
        EXPECT_EQ(WorldSaveService::waiting_save_writers(), 0u);
    }
}

TEST(WorldSaveRegionFormat, IndexedReadersWaitForRollbackBeforeReading) {
    namespace P = Luminumbra::Persistence;
    TempSaveDir save("indexed_read_rollback");
    WorldStreamingState state;
    const auto chunk = state.get_or_create_chunk({0, 0, 0});
    chunk->set_state(ChunkState::Idle);
    chunk->sdf_data.assign(17u * 17u * 17u, -1.0f);
    chunk->mark_voxel_data_dirty();
    {
        P::SaveTransaction baseline(save.path, 1, {}, &WorldSaveService::member_digest);
        ASSERT_TRUE(baseline.begin(nullptr));
        ASSERT_TRUE(WorldSaveService{}.save_world(state, save.path));
        ASSERT_TRUE(baseline.commit(nullptr));
    }
    P::SaveTransaction transaction(save.path, 2, {}, &WorldSaveService::member_digest);
    ASSERT_TRUE(transaction.begin(nullptr));
    chunk->sdf_data.front() = -9.0f;
    chunk->mark_voxel_data_dirty();
    ASSERT_EQ(WorldSaveService{}.save_dirty_chunks(state, save.path).regions_written, 1u);
    std::atomic<bool> finished{false};
    WorldSaveService::ChunkReadResult result;
    std::jthread reader([&] {
        result = WorldSaveService::read_chunk(save.path, {0, 0, 0}, true);
        finished.store(true, std::memory_order_release);
    });
    while (WorldSaveService::waiting_save_writers() == 0 &&
           !finished.load(std::memory_order_acquire))
        std::this_thread::yield();
    EXPECT_FALSE(finished.load(std::memory_order_acquire));
    EXPECT_EQ(WorldSaveService::waiting_save_writers(), 1u);
    std::vector<std::string> errors;
    transaction.abort(&errors);
    reader.join();
    EXPECT_TRUE(errors.empty());
    ASSERT_EQ(result.status, WorldSaveService::ChunkReadStatus::Found);
    ASSERT_NE(result.chunk, nullptr);
    ASSERT_FALSE(result.chunk->sdf_data.empty());
    EXPECT_EQ(result.chunk->sdf_data.front(), -1.0f);
    EXPECT_TRUE(chunk->is_voxel_data_dirty());
    EXPECT_EQ(WorldSaveService::waiting_save_writers(), 0u);
}

TEST(WorldSaveRegionFormat, IndexedWriterWaitsForAnAlreadyActiveReader) {
    namespace P = Luminumbra::Persistence;
    TempSaveDir save("indexed_writer_wait");
    WorldStreamingState state;
    const auto chunk = state.get_or_create_chunk({0, 0, 0});
    chunk->set_state(ChunkState::Idle);
    chunk->sdf_data.assign(17u * 17u * 17u, -1.0f);
    chunk->mark_voxel_data_dirty();
    {
        P::SaveTransaction baseline(save.path, 1, {}, &WorldSaveService::member_digest);
        ASSERT_TRUE(baseline.begin(nullptr));
        ASSERT_TRUE(WorldSaveService{}.save_world(state, save.path));
        ASSERT_TRUE(baseline.commit(nullptr));
    }
    PauseSavedRead pause(WorldSaveService::region_file_path(save.path, 0, 0));
    WorldSaveService::ChunkReadResult result;
    std::jthread reader([&] { result = WorldSaveService::read_chunk(save.path, {0, 0, 0}, true); });
    pause.opened.wait();
    std::atomic<bool> ownership_acquired{false};
    std::atomic<bool> begun{false};
    bool began_ok = false;
    std::vector<std::string> errors;
    std::jthread writer([&] {
        const WorldSaveService::SaveOwnership ownership;
        ownership_acquired.store(true, std::memory_order_release);
        P::SaveTransaction transaction(save.path, 2, {}, &WorldSaveService::member_digest);
        began_ok = transaction.begin(&errors);
        begun.store(true, std::memory_order_release);
        transaction.abort(&errors);
    });
    while (WorldSaveService::waiting_save_writers() == 0 && !begun.load(std::memory_order_acquire))
        std::this_thread::yield();
    // A reader permits writer ownership (and its joins), but excludes indexed publication.
    EXPECT_TRUE(ownership_acquired.load(std::memory_order_acquire));
    EXPECT_FALSE(begun.load(std::memory_order_acquire));
    EXPECT_EQ(WorldSaveService::waiting_save_writers(), 1u);
    pause.release.count_down();
    reader.join();
    writer.join();
    EXPECT_TRUE(began_ok);
    EXPECT_TRUE(errors.empty());
    ASSERT_EQ(result.status, WorldSaveService::ChunkReadStatus::Found);
    ASSERT_NE(result.chunk, nullptr);
    ASSERT_FALSE(result.chunk->sdf_data.empty());
    EXPECT_EQ(result.chunk->sdf_data.front(), -1.0f);
    EXPECT_FALSE(P::journal_pending(save.path));
    EXPECT_EQ(WorldSaveService::waiting_save_writers(), 0u);
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
            WorldSaveService::SaveFormat format;
            errors.clear();
            EXPECT_EQ(WorldSaveService::read_save_format(world, format, &errors), !over);
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

TEST(WorldSaveRegionFormat, ColdValidationTransfersRetainedDecodesOnlyForExactRecordBytes) {
    namespace P = Luminumbra::Persistence;
    TempSaveDir save("validation_transfer");
    WorldStreamingState source;
    const auto original = source.get_or_create_chunk({7, 0, 9});
    original->sdf_data.assign(17 * 17 * 17, -7.8125f);
    WorldSaveService::ContainerRecord record;
    record.id = original->get_id();
    record.flags = 1;
    record.payload = P::SerializeWorldStreamingStateSnapshotJson(source);
    const auto path = WorldSaveService::region_file_path(save.path, 0, 0);
    ASSERT_TRUE(WorldSaveService::upsert_container_records(path, {record}));
    const auto bytes = ReadFileBytes(path);
    const auto decodes = WorldSaveService::chunk_record_decode_count_for_testing();
    const WorldSaveService::WorldReadScope read_scope;
    std::vector<std::string> errors;
    ASSERT_TRUE(WorldSaveService::validate_save(save.path, &errors));
    {
        WorldSaveService::SaveFactsScope facts(save.path);
        ASSERT_TRUE(facts.validate(&errors));
    }
    EXPECT_EQ(WorldSaveService::chunk_record_decode_count_for_testing(), decodes + 1);
    WorldStreamingState loaded;
    const std::unordered_set<Luminumbra::ChunkID> wanted{original->get_id()};
    ASSERT_TRUE(WorldSaveService{}.load_world(loaded, save.path, errors, {}, &wanted));
    EXPECT_EQ(WorldSaveService::chunk_record_decode_count_for_testing(), decodes + 1);
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded.find_chunk(original->get_id())->sdf_data, original->sdf_data);
    EXPECT_EQ(ReadFileBytes(path), bytes);
    // An adopted chunk is no longer a cache entry that can be handed to another reader.
    loaded.find_chunk(original->get_id())->sdf_data.front() = 99.0f;
    WorldStreamingState another;
    ASSERT_TRUE(WorldSaveService{}.load_world(another, save.path, errors, {}, &wanted));
    EXPECT_EQ(another.find_chunk(original->get_id())->sdf_data.front(), -7.8125f);

    // A new cold validation must not lend its chunk after even one compressed byte changes.
    original->sdf_data.front() = -8.8125f;
    record.payload = P::SerializeWorldStreamingStateSnapshotJson(source);
    ASSERT_TRUE(WorldSaveService::upsert_container_records(path, {record}));
    ASSERT_TRUE(WorldSaveService::validate_save(save.path, &errors));
    auto damaged = ReadFileBytes(path);
    ASSERT_GT(damaged.size(), 28u);
    damaged[26] = 0; // LZ4 token followed by an invalid offset, independent of JSON validity.
    damaged[27] = 0;
    damaged[28] = 0;
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(damaged.data(), static_cast<std::streamsize>(damaged.size()));
        ASSERT_TRUE(output.good());
    }
    errors.clear();
    EXPECT_FALSE(WorldSaveService{}.load_world(loaded, save.path, errors, {}, &wanted));
    EXPECT_FALSE(errors.empty());
    EXPECT_EQ(loaded.size(), 0u);
}

namespace {

// Writes one region record whose header flag and payload ownership are chosen independently.
void WriteRecordWithOwnership(const std::filesystem::path& save_path,
                              bool header_edited,
                              bool terrain_edited,
                              bool water_owned) {
    namespace P = Luminumbra::Persistence;
    WorldStreamingState source;
    const auto chunk = source.get_or_create_chunk({7, 0, 9});
    chunk->sdf_data.assign(17 * 17 * 17, -7.8125f);
    if (terrain_edited) {
        chunk->mark_voxel_data_dirty();
    }
    if (water_owned) {
        chunk->mark_water_data_dirty();
    }
    WorldSaveService::ContainerRecord record;
    record.id = chunk->get_id();
    record.flags = header_edited ? 1u : 0u;
    record.payload = P::SerializeChunkAuthoritySnapshotJson(*chunk);
    ASSERT_TRUE(WorldSaveService::upsert_container_records(
        WorldSaveService::region_file_path(save_path, 0, 0), {record}));
}

bool LoadOwnershipRecord(const std::filesystem::path& save_path,
                         std::size_t& loaded_count,
                         bool& water_authority) {
    const WorldSaveService::WorldReadScope read_scope;
    WorldStreamingState loaded;
    std::vector<std::string> errors;
    const std::unordered_set<Luminumbra::ChunkID> wanted{
        Luminumbra::Chunk::calculate_id({7, 0, 9})};
    const bool ok = WorldSaveService{}.load_world(loaded, save_path, errors, {}, &wanted);
    loaded_count = loaded.size();
    const auto chunk = loaded.find_chunk(*wanted.begin());
    water_authority = chunk && chunk->has_water_authority();
    return ok;
}

} // namespace

TEST(WorldSaveRegionFormat, RecordHeaderRefusesTerrainOwnershipItDoesNotCarry) {
    {
        TempSaveDir save("record_terrain_contradiction");
        WriteRecordWithOwnership(save.path, false, true, false);
        std::vector<std::string> errors;
        EXPECT_FALSE(WorldSaveService::validate_save(save.path, &errors));
        std::size_t loaded_count = 0;
        bool water_authority = false;
        EXPECT_FALSE(LoadOwnershipRecord(save.path, loaded_count, water_authority));
        EXPECT_EQ(loaded_count, 0u);
    }
    {
        TempSaveDir save("record_terrain_agreement");
        WriteRecordWithOwnership(save.path, true, true, false);
        std::vector<std::string> errors;
        EXPECT_TRUE(WorldSaveService::validate_save(save.path, &errors));
        std::size_t loaded_count = 0;
        bool water_authority = false;
        EXPECT_TRUE(LoadOwnershipRecord(save.path, loaded_count, water_authority));
        EXPECT_EQ(loaded_count, 1u);
    }
}

TEST(WorldSaveRegionFormat, UnflaggedHeaderStillKeepsOwnedWater) {
    TempSaveDir save("record_unflagged_water");
    WriteRecordWithOwnership(save.path, false, false, true);
    std::vector<std::string> errors;
    EXPECT_TRUE(WorldSaveService::validate_save(save.path, &errors));
    std::size_t loaded_count = 0;
    bool water_authority = false;
    EXPECT_TRUE(LoadOwnershipRecord(save.path, loaded_count, water_authority));
    EXPECT_EQ(loaded_count, 1u);
    EXPECT_TRUE(water_authority);
}

TEST(WorldSaveRegionFormat, ColdValidationRetainsOnlyBoundedDecodesForTheAdoptingRead) {
    namespace P = Luminumbra::Persistence;
    struct RestoreLimit {
        ~RestoreLimit() {
            WorldSaveService::set_pending_chunk_admission_byte_limit_for_testing(std::size_t{256} *
                                                                                 1024 * 1024);
        }
    } const restore_limit;
    TempSaveDir save("bounded_validation_transfer");
    const auto path = WorldSaveService::region_file_path(save.path, 0, 0);
    std::vector<std::shared_ptr<Luminumbra::Chunk>> originals;
    std::unordered_set<Luminumbra::ChunkID> wanted;
    // Fresh record bytes each round, so every validation below starts cold.
    const auto write_records = [&](float base) {
        std::vector<WorldSaveService::ContainerRecord> records;
        originals.clear();
        wanted.clear();
        for (int x = 1; x <= 3; ++x) {
            WorldStreamingState source;
            const auto chunk = source.get_or_create_chunk({x, 0, 1});
            chunk->sdf_data.assign(17 * 17 * 17, base - static_cast<float>(x));
            WorldSaveService::ContainerRecord record;
            record.id = chunk->get_id();
            record.flags = 1;
            record.payload = P::SerializeWorldStreamingStateSnapshotJson(source);
            records.push_back(std::move(record));
            originals.push_back(chunk);
            wanted.insert(chunk->get_id());
        }
        return WorldSaveService::upsert_container_records(path, records);
    };
    const auto expect_loaded = [&](const WorldStreamingState& loaded) {
        ASSERT_EQ(loaded.size(), originals.size());
        for (const auto& original : originals) {
            const auto chunk = loaded.find_chunk(original->get_id());
            ASSERT_NE(chunk, nullptr);
            EXPECT_EQ(chunk->sdf_data, original->sdf_data);
        }
    };
    std::vector<std::string> errors;

    ASSERT_TRUE(write_records(-1.0f));
    std::size_t retained_bytes = 0;
    {
        const auto decodes = WorldSaveService::chunk_record_decode_count_for_testing();
        const WorldSaveService::WorldReadScope scope;
        ASSERT_TRUE(WorldSaveService::validate_save(save.path, &errors));
        EXPECT_EQ(WorldSaveService::pending_chunk_admission_count_for_testing(), 3u);
        retained_bytes = WorldSaveService::pending_chunk_admission_bytes_for_testing();
        EXPECT_GT(retained_bytes, 0u);
        WorldStreamingState loaded;
        ASSERT_TRUE(WorldSaveService{}.load_world(loaded, save.path, errors, {}, &wanted));
        expect_loaded(loaded);
        EXPECT_EQ(WorldSaveService::pending_chunk_admission_count_for_testing(), 0u);
        EXPECT_EQ(WorldSaveService::pending_chunk_admission_bytes_for_testing(), 0u);
        EXPECT_EQ(WorldSaveService::chunk_record_decode_count_for_testing(), decodes + 3);
    }

    // Below the retained total, validation holds fewer decodes and stays within the bound.
    const std::size_t limit = retained_bytes * 2 / 3;
    WorldSaveService::set_pending_chunk_admission_byte_limit_for_testing(limit);
    ASSERT_TRUE(write_records(-11.0f));
    {
        const WorldSaveService::WorldReadScope scope;
        ASSERT_TRUE(WorldSaveService::validate_save(save.path, &errors));
        EXPECT_LT(WorldSaveService::pending_chunk_admission_count_for_testing(), 3u);
        EXPECT_LE(WorldSaveService::pending_chunk_admission_bytes_for_testing(), limit);
        WorldStreamingState loaded;
        ASSERT_TRUE(WorldSaveService{}.load_world(loaded, save.path, errors, {}, &wanted));
        expect_loaded(loaded);
    }

    // With no room, a validation-only pass holds nothing; the adopting read is still exact.
    WorldSaveService::set_pending_chunk_admission_byte_limit_for_testing(0);
    ASSERT_TRUE(write_records(-21.0f));
    {
        const WorldSaveService::WorldReadScope scope;
        ASSERT_TRUE(WorldSaveService::validate_save(save.path, &errors));
        EXPECT_EQ(WorldSaveService::pending_chunk_admission_count_for_testing(), 0u);
        EXPECT_EQ(WorldSaveService::pending_chunk_admission_bytes_for_testing(), 0u);
        WorldStreamingState loaded;
        ASSERT_TRUE(WorldSaveService{}.load_world(loaded, save.path, errors, {}, &wanted));
        expect_loaded(loaded);
    }
    EXPECT_TRUE(errors.empty());
}

TEST(WorldSaveRegionFormat, ColdUnretainedDecodeTransfersFromPreparationToActivation) {
    namespace P = Luminumbra::Persistence;
    TempSaveDir save("activation_transfer");
    WorldStreamingState source;
    const Luminumbra::IVec3 coords{65, 0, 70};
    const auto chunk = source.get_or_create_chunk(coords);
    chunk->sdf_data.assign(17 * 17 * 17, -13.8125f);
    WorldSaveService::ContainerRecord record;
    record.id = chunk->get_id();
    record.flags = 1;
    record.payload = P::SerializeWorldStreamingStateSnapshotJson(source);
    const auto path = WorldSaveService::region_file_path(save.path, 2, 2);
    ASSERT_TRUE(WorldSaveService::upsert_container_records(path, {record}));
    const auto decodes = WorldSaveService::chunk_record_decode_count_for_testing();
    bool prepared = false;
    std::vector<std::string> errors;
    std::thread worker([&] {
        const WorldSaveService::WorldReadScope scope;
        WorldStreamingState initial;
        const std::unordered_set<Luminumbra::ChunkID> retain_none;
        std::vector<Luminumbra::IVec3> inventory;
        prepared = WorldSaveService::validate_save(save.path, &errors) &&
                   WorldSaveService{}.load_world(
                       initial, save.path, errors, {}, &retain_none, &inventory) &&
                   initial.size() == 0 && inventory.size() == 1;
    });
    worker.join();
    ASSERT_TRUE(prepared);
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(WorldSaveService::chunk_record_decode_count_for_testing(), decodes + 1);
    const auto activated = WorldSaveService::read_chunk(save.path, coords, false);
    ASSERT_EQ(activated.status, WorldSaveService::ChunkReadStatus::Found);
    ASSERT_NE(activated.chunk, nullptr);
    EXPECT_EQ(activated.chunk->sdf_data, chunk->sdf_data);
    EXPECT_EQ(WorldSaveService::chunk_record_decode_count_for_testing(), decodes + 1);
    activated.chunk->sdf_data.front() = 42.0f;
    const auto fresh = WorldSaveService::read_chunk(save.path, coords, false);
    ASSERT_EQ(fresh.status, WorldSaveService::ChunkReadStatus::Found);
    ASSERT_NE(fresh.chunk, nullptr);
    EXPECT_EQ(fresh.chunk->sdf_data.front(), -13.8125f);
    EXPECT_EQ(WorldSaveService::chunk_record_decode_count_for_testing(), decodes + 2);
}

TEST(WorldSaveRegionFormat, UnchangedSnapshotReusesSerializationAndPreservesPublishedRegion) {
    namespace P = Luminumbra::Persistence;
    TempSaveDir save("reuse_serialization");
    WorldStreamingState state;
    const auto chunk = state.get_or_create_chunk({0, 0, 0});
    chunk->sdf_data.assign(17 * 17 * 17, -9.625f);
    chunk->mark_voxel_data_dirty();
    const auto region = WorldSaveService::region_file_path(save.path, 0, 0);
    ASSERT_TRUE(WorldSaveService{}.save_world(state, save.path));
    const auto bytes = ReadFileBytes(region);
    WorldSaveService::ArtifactIdentity before{}, after{};
    bool missing = false;
    ASSERT_TRUE(WorldSaveService::artifact_identity(region, before, missing));
    ASSERT_FALSE(missing);
    const auto compressions = WorldSaveService::record_compression_count_for_testing();
    const auto decodes = WorldSaveService::chunk_record_decode_count_for_testing();
    ASSERT_TRUE(WorldSaveService{}.save_world(state, save.path));
    EXPECT_EQ(WorldSaveService::record_compression_count_for_testing(), compressions);
    EXPECT_EQ(WorldSaveService::chunk_record_decode_count_for_testing(), decodes);
    ASSERT_TRUE(WorldSaveService::artifact_identity(region, after, missing));
    EXPECT_EQ(before, after);
    EXPECT_EQ(ReadFileBytes(region), bytes);

    // These writes deliberately bypass voxel/water revision counters. Every persisted input,
    // including presentation bookkeeping, must still defeat serialization reuse.
    const std::vector<std::function<void()>> edits{
        [&] { chunk->sdf_data.front() = -2.0f; },
        [&] { chunk->material_data = {3}; },
        [&] { chunk->heightmap_data = {2.0f}; },
        [&] { chunk->mesh_vertices.push_back({Vec3(1), Vec3(0, 1, 0), 2}); },
        [&] { chunk->mesh_vertices.front().normal.x = 0.5f; },
        [&] { chunk->mesh_indices = {0}; },
        [&] { chunk->has_collision.store(true); },
        [&] { chunk->set_state(ChunkState::Meshing); },
        [&] { chunk->pending_mesh_ready.store(true); },
        [&] { chunk->mesh_version.fetch_add(1); },
        [&] { chunk->water_depth_mm = {17}; },
        [&] { chunk->water_edge_flux = {2, 3}; },
        [&] { chunk->max_water_delta_last_tick = 0.25f; },
        [&] { chunk->ticks_below_threshold = 7; },
        [&] { chunk->set_water_authority(); },
    };
    for (const auto& edit : edits) {
        edit();
        // Region records store the compact encoded form; the canonical snapshot is unchanged.
        const auto expected = P::SerializeChunkRecordPayloadJson(*chunk);
        const auto count = WorldSaveService::record_compression_count_for_testing();
        ASSERT_TRUE(WorldSaveService{}.save_world(state, save.path));
        EXPECT_EQ(WorldSaveService::record_compression_count_for_testing(), count + 1);
        std::vector<WorldSaveService::ContainerRecord> records;
        ASSERT_TRUE(WorldSaveService::read_container_records(region, records));
        ASSERT_EQ(records.size(), 1u);
        EXPECT_EQ(records.front().payload, expected);
        ASSERT_TRUE(WorldSaveService{}.save_world(state, save.path));
        EXPECT_EQ(WorldSaveService::record_compression_count_for_testing(), count + 1);
    }
}

TEST(WorldSaveRegionFormat, UnchangedIndexedRegionIsNotReadmittedAtCommit) {
    namespace P = Luminumbra::Persistence;
    TempSaveDir save("unchanged_indexed");
    WorldStreamingState state;
    const auto chunk = state.get_or_create_chunk({0, 0, 0});
    chunk->sdf_data.assign(17 * 17 * 17, -6.125f);
    chunk->mark_voxel_data_dirty();
    {
        P::SaveTransaction transaction(save.path, 1, {}, &WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(WorldSaveService{}.save_world(state, save.path));
        ASSERT_TRUE(transaction.commit(nullptr));
    }
    P::SnapshotIndex previous;
    ASSERT_TRUE(P::read_snapshot_index(save.path, previous, nullptr));
    const auto region = WorldSaveService::region_file_path(save.path, 0, 0);
    const auto bytes = ReadFileBytes(region);
    const auto manifest = WorldSaveService::world_manifest_path(save.path);
    Luminumbra::Persistence::SavedInputFile::Version manifest_before{}, manifest_after{};
    bool missing = false;
    ASSERT_TRUE(WorldSaveService::artifact_version(manifest, manifest_before, missing));
    ASSERT_FALSE(missing);
    P::SaveTransaction transaction(save.path, 2, {}, &WorldSaveService::member_digest);
    ASSERT_TRUE(transaction.begin(nullptr));
    WorldSaveService::PreparedWorldWrite prepared;
    ASSERT_TRUE(prepared.prepare(state, save.path, false));
    const auto reads = WorldSaveService::region_read_count_for_testing();
    const auto decodes = WorldSaveService::chunk_record_decode_count_for_testing();
    const auto compressions = WorldSaveService::record_compression_count_for_testing();
    ASSERT_TRUE(prepared.publish());
    ASSERT_TRUE(transaction.commit(nullptr));
    EXPECT_EQ(WorldSaveService::region_read_count_for_testing(), reads);
    EXPECT_EQ(WorldSaveService::chunk_record_decode_count_for_testing(), decodes);
    EXPECT_EQ(WorldSaveService::record_compression_count_for_testing(), compressions);
    EXPECT_EQ(ReadFileBytes(region), bytes);
    ASSERT_TRUE(WorldSaveService::artifact_version(manifest, manifest_after, missing));
    EXPECT_EQ(manifest_before, manifest_after);
    P::SnapshotIndex next;
    ASSERT_TRUE(P::read_snapshot_index(save.path, next, nullptr));
    EXPECT_EQ(next.generation, previous.generation + 1);
    ASSERT_EQ(next.members.size(), previous.members.size());
    for (std::size_t i = 0; i < previous.members.size(); ++i) {
        EXPECT_EQ(next.members[i].path, previous.members[i].path);
        EXPECT_EQ(next.members[i].generation, previous.members[i].generation);
        EXPECT_EQ(next.members[i].bytes, previous.members[i].bytes);
        EXPECT_EQ(next.members[i].fnv64, previous.members[i].fnv64);
    }
}

TEST(WorldSaveRegionFormat, DigestAuthorityReusesReadsAndDecodesUntilPublicationOrExternalChange) {
    namespace P = Luminumbra::Persistence;
    TempSaveDir save("digest_cache");
    WorldStreamingState state;
    const auto chunk = state.get_or_create_chunk({0, 0, 0});
    chunk->sdf_data.assign(17 * 17 * 17, -3.875f);
    chunk->mark_voxel_data_dirty();
    chunk->set_state(ChunkState::Idle);
    ASSERT_TRUE(WorldSaveService{}.save_world(state, save.path));
    const P::RegionDigestInputs inputs{{}, {}, {}, false};
    const auto first = WorldSaveService::region_simulation_digest(save.path, {0, 0}, inputs);
    const auto reads = WorldSaveService::region_read_count_for_testing();
    const auto decodes = WorldSaveService::chunk_record_decode_count_for_testing();
    EXPECT_EQ(WorldSaveService::region_simulation_digest(save.path, {0, 0}, inputs), first);
    EXPECT_EQ(WorldSaveService::region_read_count_for_testing(), reads);
    EXPECT_EQ(WorldSaveService::chunk_record_decode_count_for_testing(), decodes);
    // Live authority is always projected afresh even when durable authority is cached.
    const P::RegionDigestInputs live{{chunk}, {}, {}, false};
    chunk->sdf_data.front() = -11.0f;
    EXPECT_NE(WorldSaveService::region_simulation_digest(save.path, {0, 0}, live), first);
    EXPECT_EQ(WorldSaveService::region_read_count_for_testing(), reads);
    EXPECT_EQ(WorldSaveService::chunk_record_decode_count_for_testing(), decodes);
    // A publication with identical content still invalidates durable admission.
    ASSERT_TRUE(WorldSaveService::write_file_durably(save.path / "probe", "published"));
    EXPECT_EQ(WorldSaveService::region_simulation_digest(save.path, {0, 0}, inputs), first);
    EXPECT_EQ(WorldSaveService::region_read_count_for_testing(), reads + 1);
    EXPECT_EQ(WorldSaveService::chunk_record_decode_count_for_testing(), decodes + 1);
    ASSERT_TRUE(std::filesystem::remove(save.path / "probe"));
    ASSERT_TRUE(WorldSaveService{}.save_world(state, save.path));
    const auto changed = WorldSaveService::region_simulation_digest(save.path, {0, 0}, inputs);
    EXPECT_NE(changed, first);
    const auto region = WorldSaveService::region_file_path(save.path, 0, 0);
    const auto timestamp = std::filesystem::last_write_time(region);
    const auto size = std::filesystem::file_size(region);
    {
        std::ofstream output(region, std::ios::binary | std::ios::app);
        output.put('\0');
        output.close();
        ASSERT_TRUE(output.good());
    }
    ASSERT_GT(std::filesystem::file_size(region), size);
    std::filesystem::last_write_time(region, timestamp);
    try {
        (void)WorldSaveService::region_simulation_digest(save.path, {0, 0}, inputs);
        FAIL() << "Expected external region corruption to be refused";
    } catch (const std::runtime_error& error) {
        EXPECT_TRUE(std::string(error.what())
                        .starts_with("region file payload lengths do not match file size: "))
            << error.what();
    }
}

TEST(WorldSaveRegionFormat, DigestAuthorityInvalidatesAfterRollback) {
    namespace P = Luminumbra::Persistence;
    TempSaveDir save("digest_rollback");
    WorldStreamingState state;
    const auto chunk = state.get_or_create_chunk({0, 0, 0});
    chunk->sdf_data.assign(17 * 17 * 17, -4.875f);
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(WorldSaveService{}.save_world(state, save.path));
    const P::RegionDigestInputs inputs{{}, {}, {}, false};
    const auto original = WorldSaveService::region_simulation_digest(save.path, {0, 0}, inputs);
    P::SaveTransaction transaction(save.path, 1, {}, &WorldSaveService::member_digest);
    ASSERT_TRUE(transaction.begin(nullptr));
    chunk->sdf_data.front() = -12.0f;
    WorldSaveService::ContainerRecord record;
    record.id = chunk->get_id();
    record.flags = 1;
    record.payload = P::SerializeWorldStreamingStateSnapshotJson(state);
    ASSERT_TRUE(WorldSaveService::upsert_container_records(
        WorldSaveService::region_file_path(save.path, 0, 0), {record}));
    EXPECT_NE(WorldSaveService::region_simulation_digest(save.path, {0, 0}, inputs), original);
    const auto reads = WorldSaveService::region_read_count_for_testing();
    const auto decodes = WorldSaveService::chunk_record_decode_count_for_testing();
    std::vector<std::string> errors;
    transaction.abort(&errors);
    EXPECT_TRUE(errors.empty());
    EXPECT_FALSE(P::journal_pending(save.path));
    EXPECT_EQ(WorldSaveService::region_simulation_digest(save.path, {0, 0}, inputs), original);
    EXPECT_EQ(WorldSaveService::region_read_count_for_testing(), reads + 1);
    EXPECT_EQ(WorldSaveService::chunk_record_decode_count_for_testing(), decodes + 1);
    EXPECT_EQ(WorldSaveService::region_simulation_digest(save.path, {0, 0}, inputs), original);
    EXPECT_EQ(WorldSaveService::region_read_count_for_testing(), reads + 1);
    EXPECT_EQ(WorldSaveService::chunk_record_decode_count_for_testing(), decodes + 1);
}

TEST(WorldSaveRegionFormat, StreamingSnapshotArraysMatchCanonicalJsonBytes) {
    namespace P = Luminumbra::Persistence;
    WorldStreamingState state;
    const auto chunk = state.get_or_create_chunk({0, 0, 0});
    auto expected = nlohmann::json::parse(P::SerializeWorldStreamingStateSnapshotJson(state));
    auto& object = expected["chunks"][0];
    std::vector<float> floats{0.0f,
                              -0.0f,
                              0.1f,
                              -17.125f,
                              std::numeric_limits<float>::denorm_min(),
                              std::numeric_limits<float>::max(),
                              std::numeric_limits<float>::infinity(),
                              std::numeric_limits<float>::quiet_NaN()};
    // More distinct values than the formatter retains, followed by earlier inputs.
    for (int i = 0; i < 2048; ++i)
        floats.push_back(static_cast<float>(i) * 0.125f);
    floats.insert(floats.end(), {0.0f, -0.0f, 0.1f, -17.125f});
    chunk->sdf_data = floats;
    chunk->heightmap_data = floats;
    chunk->water_level_data = floats;
    chunk->water_sim_terrain_height = floats;
    chunk->material_data = {0, 1, 255};
    chunk->water_bed_mm = {0, -1, std::numeric_limits<std::int32_t>::min()};
    chunk->water_depth_mm = {1, 2, std::numeric_limits<std::int32_t>::max()};
    chunk->water_edge_flux = {-4, 3};
    chunk->mesh_vertices = {{Vec3(-0.0f, 0.1f, -17.125f), Vec3(0, 1, 0), 7},
                            {Vec3(1, 2, 3), Vec3(0.25f, -0.5f, 1), 0}};
    chunk->water_mesh_vertices = chunk->mesh_vertices;
    chunk->pending_mesh_vertices = chunk->mesh_vertices;
    chunk->pending_water_mesh_vertices = chunk->mesh_vertices;
    chunk->mesh_indices = {0, 1, std::numeric_limits<std::uint32_t>::max()};
    chunk->water_mesh_indices = chunk->mesh_indices;
    chunk->pending_mesh_indices = chunk->mesh_indices;
    chunk->pending_water_mesh_indices = chunk->mesh_indices;
    chunk->water_flow_data = {Vec2(-0.0f, 0.1f), Vec2(3, -4)};
    const auto vec3 = [](const Vec3& v) {
        return nlohmann::json{{"x", v.x}, {"y", v.y}, {"z", v.z}};
    };
    nlohmann::json vertices = nlohmann::json::array();
    for (const auto& vertex : chunk->mesh_vertices)
        vertices.push_back({{"position", vec3(vertex.position)},
                            {"normal", vec3(vertex.normal)},
                            {"material_id", vertex.material_id}});
    for (const auto* key :
         {"sdf_data", "heightmap_data", "water_level_data", "water_sim_terrain_height"})
        object[key] = floats;
    for (const auto* key : {"mesh_vertices",
                            "water_mesh_vertices",
                            "pending_mesh_vertices",
                            "pending_water_mesh_vertices"})
        object[key] = vertices;
    for (const auto* key : {"mesh_indices",
                            "water_mesh_indices",
                            "pending_mesh_indices",
                            "pending_water_mesh_indices"})
        object[key] = chunk->mesh_indices;
    object["material_data"] = chunk->material_data;
    object["water_bed_mm"] = chunk->water_bed_mm;
    object["water_depth_mm"] = chunk->water_depth_mm;
    object["water_edge_flux"] = chunk->water_edge_flux;
    object["water_flow_data"] = nlohmann::json::array();
    for (const auto& flow : chunk->water_flow_data)
        object["water_flow_data"].push_back({{"x", flow.x}, {"y", flow.y}});
    EXPECT_EQ(P::SerializeWorldStreamingStateSnapshotJson(state), expected.dump(4) + "\n");
    EXPECT_EQ(P::SerializeChunkSnapshotJson(*chunk), expected.dump(4) + "\n");
    EXPECT_EQ(P::SerializeChunkSnapshotJson(*chunk),
              P::SerializeWorldStreamingStateSnapshotJson(state));
    state.clear();
    expected["chunks"] = nlohmann::json::array();
    expected["chunk_count"] = 0;
    EXPECT_EQ(P::SerializeWorldStreamingStateSnapshotJson(state), expected.dump(4) + "\n");
}

namespace {

// Every encoded array of a region chunk record, holding the float and integer edge values.
std::shared_ptr<Chunk> EdgeValueChunk(WorldStreamingState& state) {
    using Limits = std::numeric_limits<float>;
    const auto chunk = state.get_or_create_chunk({3, -1, 5});
    std::vector<float> floats{0.0f,
                              -0.0f,
                              0.1f,
                              -17.125f,
                              Limits::denorm_min(),
                              -Limits::denorm_min(),
                              Limits::min(),
                              Limits::max(),
                              Limits::lowest(),
                              Limits::epsilon()};
    while (floats.size() < 17u * 17u * 17u)
        floats.push_back(static_cast<float>(floats.size() % 97u) * 0.173f - 8.0f);
    chunk->sdf_data = floats;
    chunk->heightmap_data.assign(floats.begin(), floats.begin() + 289);
    chunk->water_level_data.assign(floats.begin(), floats.begin() + 256);
    chunk->water_sim_terrain_height.assign(floats.rbegin(), floats.rbegin() + 256);
    chunk->material_data = {0, 1, 127, 128, 255};
    chunk->water_depth_mm = {0,
                             1,
                             -1,
                             std::numeric_limits<std::int32_t>::min(),
                             std::numeric_limits<std::int32_t>::max()};
    chunk->water_bed_mm = {std::numeric_limits<std::int32_t>::min(), 0, 7};
    chunk->water_edge_flux = {-4, 3, std::numeric_limits<std::int32_t>::max(), 0};
    chunk->mesh_vertices = {{Vec3(-0.0f, Limits::denorm_min(), Limits::max()), Vec3(0, 1, 0), 0},
                            {Vec3(Limits::lowest(), 0.1f, -17.125f),
                             Vec3(0.25f, -0.5f, 1),
                             std::numeric_limits<std::uint32_t>::max()}};
    chunk->water_mesh_vertices = {chunk->mesh_vertices.back()};
    chunk->pending_mesh_vertices = chunk->mesh_vertices;
    chunk->mesh_indices = {0, 1, std::numeric_limits<std::uint32_t>::max()};
    chunk->water_mesh_indices = {2};
    chunk->pending_mesh_indices = chunk->mesh_indices;
    chunk->water_flow_data = {Vec2(-0.0f, -Limits::denorm_min()), Vec2(3, -4)};
    chunk->max_water_delta_last_tick = -0.0f;
    chunk->ticks_below_threshold = 7;
    chunk->water_mesh_dirty_ticks = -3;
    chunk->mesh_version.store(9);
    chunk->set_state(ChunkState::Ready);
    // A decoded record is loaded authority; match it so the raw inputs compare whole.
    chunk->mark_voxel_data_dirty();
    return chunk;
}

} // namespace

TEST(WorldSaveRegionFormat, ChunkRecordPayloadRoundTripsEveryArrayBitExactly) {
    namespace P = Luminumbra::Persistence;
    WorldStreamingState state;
    const auto chunk = EdgeValueChunk(state);
    const auto terrain_hash = P::ComputeWorldStreamingStateHash(state);
    const std::string payload = P::SerializeChunkRecordPayloadJson(*chunk);
    EXPECT_EQ(payload.find('\n'), std::string::npos);
    const auto document = nlohmann::json::parse(payload);
    EXPECT_EQ(document.at("record_format"), 2);
    const auto& stored = document.at("chunks").at(0);
    const std::vector<std::pair<const char*, const char*>> types{
        {"sdf_data", "f32"},
        {"heightmap_data", "f32"},
        {"material_data", "u8"},
        {"mesh_vertices", "vertex"},
        {"mesh_indices", "u32"},
        {"water_mesh_vertices", "vertex"},
        {"water_mesh_indices", "u32"},
        {"pending_mesh_vertices", "vertex"},
        {"pending_mesh_indices", "u32"},
        {"pending_water_mesh_vertices", "vertex"},
        {"pending_water_mesh_indices", "u32"},
        {"water_level_data", "f32"},
        {"water_flow_data", "f32x2"},
        {"water_sim_terrain_height", "f32"},
        {"water_depth_mm", "i32"},
        {"water_bed_mm", "i32"},
        {"water_edge_flux", "i32"}};
    for (const auto& [field, type] : types) {
        SCOPED_TRACE(field);
        ASSERT_TRUE(stored.at(field).is_object());
        EXPECT_EQ(stored.at(field).at("enc"), "le-b64");
        EXPECT_EQ(stored.at(field).at("type"), type);
    }
    EXPECT_EQ(stored.at("sdf_data").at("count"), chunk->sdf_data.size());
    EXPECT_EQ(stored.at("mesh_vertices").at("count"), 2u);
    // Explicit little-endian bytes: -0.0f is 00 00 00 80, the negative denormal 01 00 00 80.
    EXPECT_EQ(stored.at("water_flow_data").at("data"), "AAAAgAEAAIAAAEBAAACAwA==");

    WorldStreamingState loaded;
    std::vector<std::string> errors;
    ASSERT_TRUE(P::LoadWorldStreamingStateSnapshotJson(payload, loaded, errors))
        << (errors.empty() ? std::string{} : errors.front());
    const auto restored = loaded.find_chunk(chunk->get_id());
    ASSERT_NE(restored, nullptr);
    // The raw bytes of every array and scalar, so signed zeros and denormals compare by bits.
    EXPECT_EQ(P::ChunkSnapshotInputBytes(*restored), P::ChunkSnapshotInputBytes(*chunk));
    EXPECT_EQ(std::bit_cast<std::uint32_t>(restored->sdf_data[1]), 0x80000000u);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(restored->sdf_data[5]), 0x80000001u);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(restored->mesh_vertices[0].position.x), 0x80000000u);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(restored->max_water_delta_last_tick), 0x80000000u);

    // Hashes and checksums read the canonical serializers, so the stored form cannot move them.
    EXPECT_EQ(P::ComputeWorldStreamingStateHash(state), terrain_hash);
    EXPECT_EQ(P::ComputeWorldStreamingStateHash(loaded), terrain_hash);
    EXPECT_EQ(P::ComputeRegionContentHash(loaded), P::ComputeRegionContentHash(state));
    EXPECT_EQ(P::SerializeWorldStreamingStateSnapshotJson(loaded),
              P::SerializeWorldStreamingStateSnapshotJson(state));
    EXPECT_EQ(P::SerializeChunkSnapshotJson(*restored), P::SerializeChunkSnapshotJson(*chunk));
    const auto sub_hashes = P::ComputeWorldStreamingStateSubHashes(state);
    const auto loaded_sub_hashes = P::ComputeWorldStreamingStateSubHashes(loaded);
    EXPECT_EQ(loaded_sub_hashes.terrain, sub_hashes.terrain);
    EXPECT_EQ(loaded_sub_hashes.mesh, sub_hashes.mesh);
    EXPECT_EQ(loaded_sub_hashes.water, sub_hashes.water);

    // The indented plain-array record of earlier saves still loads to the same values.
    const std::string legacy = P::SerializeWorldStreamingStateSnapshotJson(state);
    EXPECT_EQ(nlohmann::json::parse(legacy).count("record_format"), 0u);
    WorldStreamingState legacy_loaded;
    ASSERT_TRUE(P::LoadWorldStreamingStateSnapshotJson(legacy, legacy_loaded, errors));
    const auto legacy_chunk = legacy_loaded.find_chunk(chunk->get_id());
    ASSERT_NE(legacy_chunk, nullptr);
    EXPECT_EQ(P::ChunkSnapshotInputBytes(*legacy_chunk), P::ChunkSnapshotInputBytes(*chunk));
    EXPECT_EQ(P::ComputeWorldStreamingStateHash(legacy_loaded), terrain_hash);
    EXPECT_TRUE(errors.empty());
}

TEST(WorldSaveRegionFormat, ChunkRecordPayloadRefusesMalformedEncodedArrays) {
    namespace P = Luminumbra::Persistence;
    WorldStreamingState state;
    const auto chunk = EdgeValueChunk(state);
    const auto valid = nlohmann::json::parse(P::SerializeChunkRecordPayloadJson(*chunk));
    const auto refused = [&](const char* expected,
                             const std::function<void(nlohmann::json&)>& damage) {
        SCOPED_TRACE(expected);
        auto document = valid;
        damage(document);
        WorldStreamingState loaded;
        loaded.get_or_create_chunk({0, 0, 0});
        std::vector<std::string> errors;
        EXPECT_FALSE(P::LoadWorldStreamingStateSnapshotJson(document.dump(), loaded, errors));
        EXPECT_EQ(loaded.size(), 0u);
        ASSERT_FALSE(errors.empty());
        EXPECT_NE(errors.front().find(expected), std::string::npos) << errors.front();
    };
    const auto field = [](nlohmann::json& document, const char* name) -> nlohmann::json& {
        return document["chunks"][0][name];
    };
    const auto data = [&field](nlohmann::json& document, const char* name) -> std::string& {
        return field(document, name)["data"].get_ref<std::string&>();
    };
    refused("not base64", [&](nlohmann::json& d) { data(d, "sdf_data")[5] = '*'; });
    refused("not base64", [&](nlohmann::json& d) { data(d, "sdf_data")[4] = '='; });
    refused("count does not match", [&](nlohmann::json& d) {
        auto& count = field(d, "sdf_data")["count"];
        count = count.get<std::uint64_t>() + 1u;
    });
    refused("count does not match", [&](nlohmann::json& d) {
        field(d, "heightmap_data")["count"] = std::numeric_limits<std::uint64_t>::max();
    });
    refused("count must be an unsigned integer",
            [&](nlohmann::json& d) { field(d, "mesh_indices")["count"] = -1; });
    refused("count does not match", [&](nlohmann::json& d) {
        auto& text = data(d, "water_depth_mm");
        text.resize(text.size() - 4u);
    });
    refused("count does not match", [&](nlohmann::json& d) { data(d, "mesh_vertices").clear(); });
    refused("wrong element type",
            [&](nlohmann::json& d) { field(d, "water_bed_mm")["type"] = "u32"; });
    refused("wrong element type",
            [&](nlohmann::json& d) { field(d, "mesh_vertices")["type"] = "f32"; });
    refused("unsupported encoding",
            [&](nlohmann::json& d) { field(d, "material_data")["enc"] = "be-b64"; });
    refused("encoded array object",
            [&](nlohmann::json& d) { field(d, "water_flow_data")["extra"] = 0; });
    refused("encoded array object",
            [&](nlohmann::json& d) { field(d, "sdf_data") = chunk->sdf_data; });
    refused("encoded array object", [&](nlohmann::json& d) { field(d, "sdf_data") = nullptr; });
    refused("water_edge_flux", [&](nlohmann::json& d) { d["chunks"][0].erase("water_edge_flux"); });
    // One byte is "AA==": a set spare bit or a misplaced pad is not canonical base64.
    refused("padding bits are not zero", [&](nlohmann::json& d) {
        field(d, "material_data") = {
            {"enc", "le-b64"}, {"type", "u8"}, {"count", 1}, {"data", "AB=="}};
    });
    refused("padding is malformed", [&](nlohmann::json& d) {
        field(d, "material_data") = {
            {"enc", "le-b64"}, {"type", "u8"}, {"count", 1}, {"data", "AAA="}};
    });
    // A quiet NaN (00 00 c0 7f) and +infinity (00 00 80 7f) have no plain-JSON form either.
    refused("non-finite", [&](nlohmann::json& d) {
        field(d, "sdf_data") = {
            {"enc", "le-b64"}, {"type", "f32"}, {"count", 1}, {"data", "AADAfw=="}};
    });
    refused("non-finite", [&](nlohmann::json& d) {
        field(d, "water_flow_data") = {
            {"enc", "le-b64"}, {"type", "f32x2"}, {"count", 1}, {"data", "AACAfwAAAAA="}};
    });
    refused("record format", [](nlohmann::json& d) { d["record_format"] = 3; });
    refused("record format", [](nlohmann::json& d) { d["record_format"] = "2"; });

    // Non-finite values are written as null, which loading refuses as before.
    chunk->sdf_data[3] = std::numeric_limits<float>::quiet_NaN();
    chunk->water_flow_data[1].y = std::numeric_limits<float>::infinity();
    chunk->mesh_vertices[0].normal.z = -std::numeric_limits<float>::infinity();
    const std::string non_finite = P::SerializeChunkRecordPayloadJson(*chunk);
    for (const char* name :
         {"\"sdf_data\":null", "\"water_flow_data\":null", "\"mesh_vertices\":null"})
        EXPECT_NE(non_finite.find(name), std::string::npos) << name;
    WorldStreamingState loaded;
    std::vector<std::string> errors;
    EXPECT_FALSE(P::LoadWorldStreamingStateSnapshotJson(non_finite, loaded, errors));
    EXPECT_FALSE(errors.empty());

    // The region decoder reports the refusal for a stored record as well.
    TempSaveDir directory("encoded_array_refusal");
    const auto path = directory.path / "container.lmr";
    auto damaged = valid;
    data(damaged, "sdf_data")[0] = '-';
    WorldSaveService::ContainerRecord record;
    record.id = chunk->get_id();
    record.flags = 1;
    record.payload = damaged.dump();
    ASSERT_TRUE(WorldSaveService::upsert_container_records(path, {record}));
    std::vector<std::shared_ptr<Chunk>> chunks;
    errors.clear();
    EXPECT_FALSE(WorldSaveService::read_region_chunks(path, chunks, &errors));
    EXPECT_TRUE(chunks.empty());
    ASSERT_FALSE(errors.empty());
    EXPECT_NE(errors.back().find("failed to decode chunk record payload"), std::string::npos);
}

TEST(WorldSaveRegionFormat, ChunkRecordPayloadIsSubstantiallySmallerThanTheIndentedSnapshot) {
    namespace P = Luminumbra::Persistence;
    WorldStreamingState state;
    const auto chunk = state.get_or_create_chunk({0, 0, 0});
    const auto value = [](std::size_t index) {
        return static_cast<float>(index % 97u) * 0.173f - 8.0f;
    };
    for (std::size_t i = 0; i < 17u * 17u * 17u; ++i)
        chunk->sdf_data.push_back(value(i));
    for (std::size_t i = 0; i < 17u * 17u; ++i) {
        chunk->heightmap_data.push_back(value(i + 3u));
        chunk->water_depth_mm.push_back(static_cast<std::int32_t>(i * 37u));
        chunk->water_bed_mm.push_back(static_cast<std::int32_t>(i * 1009u) - 5000);
    }
    chunk->water_edge_flux.assign(chunk->water_depth_mm.size() * 2u, -3);
    for (std::size_t i = 0; i < 1200u; ++i) {
        chunk->mesh_vertices.push_back(
            {Vec3(value(i), value(i + 1u), value(i + 2u)), Vec3(0.6f, 0.8f, 0.0f), 2u});
        chunk->mesh_indices.push_back(static_cast<std::uint32_t>(i));
    }
    chunk->mark_voxel_data_dirty();
    const std::string indented = P::SerializeChunkSnapshotJson(*chunk);
    const std::string encoded = P::SerializeChunkRecordPayloadJson(*chunk);
    // The indented record spends 20+ bytes per number; four bytes become 5.33 base64 bytes.
    EXPECT_LT(encoded.size() * 4u, indented.size()) << encoded.size() << " vs " << indented.size();
    WorldStreamingState loaded;
    std::vector<std::string> errors;
    ASSERT_TRUE(P::LoadWorldStreamingStateSnapshotJson(encoded, loaded, errors));
    EXPECT_EQ(P::ChunkSnapshotInputBytes(*loaded.find_chunk(chunk->get_id())),
              P::ChunkSnapshotInputBytes(*chunk));
}

TEST(WorldSaveRegionFormat, LoadThenSaveRewritesNoUnchangedRecord) {
    namespace P = Luminumbra::Persistence;
    TempSaveDir save("load_then_save");
    const auto path = WorldSaveService::region_file_path(save.path, 0, 0);
    // Plain-array records, as earlier builds stored them.
    std::vector<WorldSaveService::ContainerRecord> records;
    std::unordered_set<Luminumbra::ChunkID> wanted;
    for (int x = 1; x <= 3; ++x) {
        WorldStreamingState source;
        const auto chunk = source.get_or_create_chunk({x, 0, 2});
        chunk->sdf_data.assign(17 * 17 * 17, -2.5f - static_cast<float>(x));
        WorldSaveService::ContainerRecord record;
        record.id = chunk->get_id();
        record.flags = 1;
        record.payload = P::SerializeWorldStreamingStateSnapshotJson(source);
        records.push_back(std::move(record));
        wanted.insert(chunk->get_id());
    }
    ASSERT_TRUE(WorldSaveService::upsert_container_records(path, records));
    std::vector<std::string> errors;
    WorldStreamingState loaded;
    ASSERT_TRUE(WorldSaveService{}.load_world(loaded, save.path, errors, {}, &wanted));
    ASSERT_EQ(loaded.size(), 3u);

    // Unchanged adopted chunks keep their stored records: nothing is encoded or rewritten.
    const auto compressions = WorldSaveService::record_compression_count_for_testing();
    ASSERT_TRUE(WorldSaveService{}.save_world(loaded, save.path, &errors))
        << (errors.empty() ? std::string{} : errors.front());
    EXPECT_EQ(WorldSaveService::record_compression_count_for_testing(), compressions);
    std::vector<WorldSaveService::ContainerRecord> kept;
    ASSERT_TRUE(WorldSaveService::read_container_records(path, kept));
    ASSERT_EQ(kept.size(), records.size());
    for (std::size_t index = 0; index < kept.size(); ++index) {
        EXPECT_EQ(kept[index].id, records[index].id);
        EXPECT_EQ(kept[index].flags, records[index].flags);
        EXPECT_EQ(kept[index].payload, records[index].payload);
    }

    // A changed chunk is encoded again, in the current format; the others stay as stored.
    const auto edited = loaded.find_chunk(Chunk::calculate_id({2, 0, 2}));
    ASSERT_NE(edited, nullptr);
    edited->sdf_data.front() = 4.0f;
    ASSERT_TRUE(WorldSaveService{}.save_world(loaded, save.path, &errors));
    EXPECT_EQ(WorldSaveService::record_compression_count_for_testing(), compressions + 1);
    std::vector<WorldSaveService::ContainerRecord> stored;
    ASSERT_TRUE(WorldSaveService::read_container_records(path, stored));
    ASSERT_EQ(stored.size(), 3u);
    for (const auto& record : stored) {
        EXPECT_EQ(nlohmann::json::parse(record.payload).contains("record_format"),
                  record.id == edited->get_id());
    }

    // Kept and rewritten records both load to the chunks that were saved.
    WorldStreamingState reloaded;
    ASSERT_TRUE(WorldSaveService{}.load_world(reloaded, save.path, errors, {}, &wanted));
    for (const auto& chunk : loaded.snapshot_chunks()) {
        const auto restored = reloaded.find_chunk(chunk->get_id());
        ASSERT_NE(restored, nullptr);
        EXPECT_EQ(P::ChunkSnapshotInputBytes(*restored), P::ChunkSnapshotInputBytes(*chunk));
    }
    EXPECT_TRUE(errors.empty());
}

TEST(WorldSaveRegionFormat, WrittenRegionDigestIsReusedOnlyWhileItsFileVersionMatches) {
    namespace P = Luminumbra::Persistence;
    for (int change = 0; change < 4; ++change) {
        SCOPED_TRACE(change);
        const bool tamper = change >= 2;
        TempSaveDir save("written_digest");
        WorldStreamingState state;
        const auto chunk = state.get_or_create_chunk({0, 0, 0});
        chunk->sdf_data.assign(17 * 17 * 17, -2.125f);
        state.get_or_create_chunk({40, 0, 0})->sdf_data.assign(17 * 17 * 17, -3.125f);
        WorldSaveService::SaveFactsScope facts(save.path);
        P::SaveTransaction transaction(save.path, 1, {}, &WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(WorldSaveService{}.save_world(state, save.path));
        const auto region = WorldSaveService::region_file_path(save.path, 0, 0);
        if (change == 1) {
            chunk->sdf_data.front() = -7.0f;
            ASSERT_TRUE(WorldSaveService{}.save_world(state, save.path));
        }
        if (change == 3) {
            static bool injected = false;
            injected = false;
            struct ResetHook {
                ~ResetHook() {
                    P::detail::set_file_publication_hook_for_testing(nullptr);
                }
            } reset;
            P::detail::set_file_publication_hook_for_testing(+[](const std::filesystem::path& path,
                                                                 bool removing) {
                if (removing || path.filename() != "r.0.0.lmr")
                    return true;
                const auto directory = WorldSaveService::region_staging_path(path).parent_path();
                for (const auto& entry : std::filesystem::directory_iterator(directory)) {
                    if (!entry.path().filename().string().starts_with("r.0.0.lmr.tmp."))
                        continue;
                    const auto& staged = entry.path();
                    const auto timestamp = std::filesystem::last_write_time(staged);
                    auto bytes = ReadFileBytes(staged);
                    bytes.front() = 'X';
                    std::ofstream output(staged, std::ios::binary | std::ios::trunc);
                    output << bytes;
                    output.close();
                    injected = output.good();
                    std::filesystem::last_write_time(staged, timestamp);
                }
                return true;
            });
            chunk->sdf_data.front() = -7.0f;
            ASSERT_TRUE(WorldSaveService{}.save_world(state, save.path));
            ASSERT_TRUE(injected);
        }
        if (change == 2) {
            const auto timestamp = std::filesystem::last_write_time(region);
            auto bytes = ReadFileBytes(region);
            bytes.front() = 'X';
            {
                std::ofstream output(region, std::ios::binary | std::ios::trunc);
                output << bytes;
                output.close();
                ASSERT_TRUE(output.good());
            }
            std::filesystem::last_write_time(region, timestamp);
        }
        const auto reads = WorldSaveService::region_read_count_for_testing();
        std::vector<std::string> errors;
        EXPECT_EQ(transaction.commit(&errors), !tamper);
        if (tamper) {
            EXPECT_GT(WorldSaveService::region_read_count_for_testing(), reads);
            EXPECT_FALSE(errors.empty());
        } else {
#if !defined(_WIN32)
            EXPECT_EQ(WorldSaveService::region_read_count_for_testing(), reads);
#endif
            P::MemberDigest actual;
            std::string error;
            ASSERT_TRUE(WorldSaveService::member_digest(
                save.path, "chunks/region/r.0.0.lmr", actual, error));
            P::SnapshotIndex index;
            ASSERT_TRUE(P::read_snapshot_index(save.path, index, nullptr));
            const auto member =
                std::find_if(index.members.begin(), index.members.end(), [](const auto& value) {
                    return value.path == "chunks/region/r.0.0.lmr";
                });
            ASSERT_NE(member, index.members.end());
            EXPECT_EQ(member->bytes, actual.bytes);
            EXPECT_EQ(member->fnv64, actual.fnv64);
            WorldStreamingState loaded;
            EXPECT_TRUE(WorldSaveService{}.load_world(loaded, save.path, errors));
            ASSERT_NE(loaded.find_chunk(chunk->get_id()), nullptr);
            EXPECT_EQ(loaded.find_chunk(chunk->get_id())->sdf_data, chunk->sdf_data);
        }
    }
}

TEST(WorldSaveRegionFormat, IdenticalContainerUpsertPreservesFileAndChangedRecordReplacesIt) {
    TempSaveDir save("upsert_same_bytes");
    const auto region = WorldSaveService::region_file_path(save.path, 0, 0);
    WorldSaveService::ContainerRecord record;
    record.id = 1;
    record.lod_level = 1;
    record.payload = "container payload";
    ASSERT_TRUE(WorldSaveService::upsert_container_records(region, {record}));
    const auto bytes = ReadFileBytes(region);
    WorldSaveService::ArtifactIdentity before{}, after{};
    bool missing = false;
    ASSERT_TRUE(WorldSaveService::artifact_identity(region, before, missing));
    ASSERT_FALSE(missing);
    ASSERT_TRUE(WorldSaveService::upsert_container_records(region, {record}));
    ASSERT_TRUE(WorldSaveService::artifact_identity(region, after, missing));
    EXPECT_EQ(before, after);
    EXPECT_EQ(ReadFileBytes(region), bytes);
    record.payload += " changed";
    ASSERT_TRUE(WorldSaveService::upsert_container_records(region, {record}));
    EXPECT_NE(ReadFileBytes(region), bytes);
    std::vector<WorldSaveService::ContainerRecord> loaded;
    ASSERT_TRUE(WorldSaveService::read_container_records(region, loaded));
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded.front().payload, record.payload);
}
