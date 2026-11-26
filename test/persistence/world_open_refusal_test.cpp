#include "../support/CalendarMetadataCases.h"
#include "../support/GameSessionTestWorld.h"
#include "../support/IndexedSaveFixture.h"
#include "../support/PersistenceFileOps.h"
#include "../support/SessionEnergyTick.h"
#include "../support/WorldSaveTestAccess.h"
#include "../support/WorldgenTestData.h"
#include "core/EngineVersion.h"
#include "luminumbra_client/app/SessionExit.h"
#include "luminumbra_client/ui/core/UIHotReload.h"
#include <chrono>
#include <cmath>
#include <gtest/gtest.h>
#include <thread>

#include "luminumbra/core/Types.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/PlantComponents.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/fields/EnergyFieldState.h"
#include "luminumbra_common/net/LockstepSession.h"
#include "luminumbra_common/persistence/SavedEnergyInput.h"
#include "luminumbra_common/persistence/SavedJsonAdmission.h"
#include "luminumbra_common/persistence/SavedWorldCatalog.h"
#include "luminumbra_common/persistence/SyntheticSaveFixture.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/replay/ReplayStream.h"
#include "luminumbra_common/systems/AetherFieldSystem.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WaterSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"
#include "luminumbra_common/world/ActiveRegionLedger.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/FarLodStore.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/world/TerrainPresetLoader.h"
#include "luminumbra_common/world/WorldStreamingState.h"
#include "luminumbra_server/ServerCliOptions.h"
#include "luminumbra_server/ServerWorldRunner.h"
#include "luminumbra_server/modes/ModeHelpers.h"
#include "luminumbra_server/modes/Modes.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
namespace fs = std::filesystem;
using namespace Luminumbra;
using Persistence::WorldSaveService;
using TestSupport::ApplySnapshotTestEnergy;
using world::GameSession;

std::string Read(const fs::path& path) {
    const std::ifstream input(path, std::ios::binary);
    std::ostringstream bytes;
    bytes << input.rdbuf();
    return bytes.str();
}
void Write(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(output.good());
}
std::map<std::string, std::string> DiskBytes(const fs::path& root) {
    std::map<std::string, std::string> result;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        const auto key = entry.path().lexically_relative(root).generic_string();
        result.emplace(key, entry.is_directory() ? "<directory>" : Read(entry.path()));
    }
    return result;
}

std::uint32_t FixtureChunksFromEnv(const char* name, std::uint32_t fallback) {
    const char* text = std::getenv(name);
    if (text == nullptr || *text == '\0')
        return fallback;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0' || value < 1 || value > 100000)
        return fallback;
    return static_cast<std::uint32_t>(value);
}

// Parallel ctest runs one process per parameterisation, and a coarse clock can hand two of
// them the same tick, so the name combines random entropy with a per-process counter and the
// directory is claimed atomically: create_directory reports false when it already existed.
fs::path UniqueRoot() {
    static std::atomic<std::uint64_t> counter{0};
    std::random_device entropy;
    for (;;) {
        const std::uint64_t suffix = (static_cast<std::uint64_t>(entropy()) << 32) ^ entropy();
        const fs::path candidate =
            fs::temp_directory_path() / ("world_open_refusal_" + std::to_string(suffix) + "_" +
                                         std::to_string(counter.fetch_add(1)));
        std::error_code ec;
        if (fs::create_directory(candidate, ec))
            return candidate;
        if (ec)
            throw fs::filesystem_error("create_directory", candidate, ec);
    }
}

void ExpectMissingPresetReason(const std::string& error, const fs::path& expected_path) {
    const std::string prefix = "failed to open world preset: ";
    ASSERT_EQ(error.rfind(prefix, 0), 0u);
    EXPECT_EQ(fs::path(error.substr(prefix.size())).lexically_normal(),
              expected_path.lexically_normal());
}

// Each pass leaves its read batch, so only the open reader keeps admission warm.
void ExpectWarmReaderPasses(const fs::path& save_dir,
                            std::uint32_t count,
                            WorldSaveService::ChunkReader& reader) {
    const auto validations = Persistence::detail::decode_count_for_testing();
    const auto compressions = WorldSaveService::record_compression_count_for_testing();
    const auto coordinates = Persistence::SyntheticChunkCoords(count);
    const auto reads_before_passes = WorldSaveService::region_read_count_for_testing();
    std::set<std::pair<int, int>> touched_regions;
    // Admit each touched region once, then leave each read pass entirely.
    for (const auto& coords : coordinates) {
        int region_x = 0, region_z = 0;
        WorldSaveService::region_coords_for_chunk(coords, region_x, region_z);
        touched_regions.emplace(region_x, region_z);
        const WorldSaveService::ChunkReadBatch pass;
        const auto read = WorldSaveService::read_chunk(save_dir, coords, true, &reader);
        ASSERT_EQ(read.status, WorldSaveService::ChunkReadStatus::Found) << read.error;
    }
    const auto region_reads = WorldSaveService::region_read_count_for_testing();
    EXPECT_EQ(region_reads, reads_before_passes + touched_regions.size());
    const auto fingerprints = WorldSaveService::region_fingerprint_count_for_testing();
    const auto admitted_parses = Persistence::saved_json_parse_count_for_testing();
    const auto admitted_decodes = WorldSaveService::chunk_record_decode_count_for_testing();
    for (std::uint64_t pass_number = 0; pass_number < 3; ++pass_number) {
        const WorldSaveService::ChunkReadBatch pass;
        for (const auto& coords : coordinates) {
            const auto read = WorldSaveService::read_chunk(save_dir, coords, true, &reader);
            ASSERT_EQ(read.status, WorldSaveService::ChunkReadStatus::Found) << read.error;
            ASSERT_TRUE(read.chunk);
            EXPECT_EQ(read.chunk->get_coords(), coords);
        }
        EXPECT_EQ(WorldSaveService::region_read_count_for_testing(), region_reads);
        EXPECT_EQ(WorldSaveService::region_fingerprint_count_for_testing(), fingerprints);
        EXPECT_EQ(Persistence::saved_json_parse_count_for_testing(), admitted_parses);
        EXPECT_EQ(WorldSaveService::chunk_record_decode_count_for_testing(),
                  admitted_decodes + (pass_number + 1) * count);
        EXPECT_EQ(Persistence::detail::decode_count_for_testing(), validations);
        EXPECT_EQ(WorldSaveService::record_compression_count_for_testing(), compressions);
    }
}

void ExpectReaderRefusesChangedBytes(const fs::path& root,
                                     const fs::path& save_dir,
                                     std::uint32_t count,
                                     WorldSaveService::ChunkReader& reader) {
    const auto coordinates = Persistence::SyntheticChunkCoords(count);
    const auto region_reads = WorldSaveService::region_read_count_for_testing();
    // Even an unrelated publication invalidates this open reader.
    const auto published = root / "publication.bin";
    ASSERT_TRUE(WorldSaveService::write_file_durably(published, "published"));
    const auto reread = WorldSaveService::read_chunk(save_dir, coordinates.front(), true, &reader);
    ASSERT_EQ(reread.status, WorldSaveService::ChunkReadStatus::Found) << reread.error;
    EXPECT_EQ(WorldSaveService::region_read_count_for_testing(), region_reads + 1);
    fs::remove(published);
    // External edits have no publication revision: fresh bytes must still refuse them.
    int rx = 0, rz = 0;
    WorldSaveService::region_coords_for_chunk(coordinates.front(), rx, rz);
    const auto region_path = WorldSaveService::region_file_path(save_dir, rx, rz);
    const auto original_region = Read(region_path);
    auto changed_region = original_region;
    ASSERT_GT(changed_region.size(), 8u);
    changed_region[8] ^= 1; // record identity, same file length
    Write(region_path, changed_region);
    {
        const WorldSaveService::ChunkReadBatch pass;
        const auto refused =
            WorldSaveService::read_chunk(save_dir, coordinates.front(), true, &reader);
        EXPECT_EQ(refused.status, WorldSaveService::ChunkReadStatus::Error);
        EXPECT_FALSE(refused.chunk);
        EXPECT_FALSE(refused.error.empty());
    }
    Write(region_path, original_region);
    changed_region = original_region;
    changed_region.back() ^= 1; // payload-only edit, with an unchanged manifest
    Write(region_path, changed_region);
    bool refused_payload = false;
    for (const auto& coords : coordinates) {
        int chunk_rx = 0, chunk_rz = 0;
        WorldSaveService::region_coords_for_chunk(coords, chunk_rx, chunk_rz);
        if (chunk_rx != rx || chunk_rz != rz)
            continue;
        const WorldSaveService::ChunkReadBatch pass;
        const auto read = WorldSaveService::read_chunk(save_dir, coords, true, &reader);
        if (read.status == WorldSaveService::ChunkReadStatus::Error) {
            refused_payload = true;
            EXPECT_FALSE(read.chunk);
            EXPECT_FALSE(read.error.empty());
        }
    }
    EXPECT_TRUE(refused_payload);
    Write(region_path, original_region);
    const auto index_path = Persistence::snapshot_index_path(save_dir);
    const auto original_index = Read(index_path);
    Write(index_path, "{");
    EXPECT_EQ(WorldSaveService::read_chunk(save_dir, coordinates.front(), true, &reader).status,
              WorldSaveService::ChunkReadStatus::Error);
    Write(index_path, original_index);
    const IVec3 absent_coords(3200, 0, 3200);
    EXPECT_EQ(WorldSaveService::read_chunk(save_dir, absent_coords, true, &reader).status,
              WorldSaveService::ChunkReadStatus::Absent);
    int absent_rx = 0, absent_rz = 0;
    WorldSaveService::region_coords_for_chunk(absent_coords, absent_rx, absent_rz);
    const auto absent_region = WorldSaveService::region_file_path(save_dir, absent_rx, absent_rz);
    Write(absent_region, "");
    EXPECT_EQ(WorldSaveService::read_chunk(save_dir, absent_coords, true, &reader).status,
              WorldSaveService::ChunkReadStatus::Error);
    fs::remove(absent_region);
}

struct WorldOpenFixtureState {
    fs::path root;
    fs::path save;
    JobSystem jobs;
};

class WorldOpenRefusal : public testing::TestWithParam<std::tuple<int, int>>,
                         protected WorldOpenFixtureState {
protected:
    void CheckLegacyFullSnapshotRefusalAndRecovery();

    void SetUp() override {
        root = UniqueRoot();
        fs::create_directories(root / "worlds/atlas/presets");
        fs::copy_file(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets/default.json",
                      root / "worlds/atlas/presets/default.json");
        TestSupport::CopyWorldgenTestData(LUMINUMBRA_SOURCE_ROOT, root);
        fs::copy(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets-rev6",
                 root / "worlds/atlas/presets-rev6",
                 fs::copy_options::recursive);
        save = root / "worlds/saves/fixture";
        Write(
            save / "world_info.json",
            R"({"container_version":2,"name":"Fixture","seed":"1337","worldType":"default","creationTime":1,"spawnPoint":{"x":8,"y":18,"z":8}})");
        jobs.startup(1);
        WorldStreamingState state;
        auto chunk = state.get_or_create_chunk(IVec3(0, 0, 0));
        chunk->sdf_data.assign(static_cast<std::size_t>(CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1) *
                                   (CHUNK_SIZE_Z + 1),
                               1.0f);
        chunk->heightmap_data.assign(
            static_cast<std::size_t>(CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Z + 1), 8.0f);
        chunk->mark_sdf_loaded_or_edited();
        ASSERT_TRUE(WorldSaveService{}.save_world(state, save));
    }
    void TearDown() override {
        jobs.shutdown();
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    std::string RootString() const {
        return root.generic_string() + "/";
    }
    void Damage(int kind) {
        const auto manifest = WorldSaveService::world_manifest_path(save);
        const auto region = WorldSaveService::region_file_path(save, 0, 0);
        auto bytes = Read(region);
        if (kind == 0 || kind == 1) {
            if (kind == 0)
                fs::remove_all(WorldSaveService::region_directory(save));
            Write(WorldSaveService::world_state_path(save), "{obsolete snapshot}");
        } else if (kind == 2) {
            Write(save / "chunks/world-state.json.bak", "obsolete backup");
        } else if (kind == 3 || kind == 4 || kind == 5) {
            bytes[4] = kind == 5 ? 3 : 1;
            if (kind == 3)
                fs::remove(manifest);
            if (kind == 4) {
                // A good region sorts first, but may never escape as partial state.
                Write(WorldSaveService::region_file_path(save, 1, 0), bytes);
            } else
                Write(region, bytes);
        } else if (kind == 6) {
            Write(region, bytes.substr(0, bytes.size() / 2));
        } else if (kind == 7) {
            Write(manifest, "{");
        } else if (kind == 8 || kind == 9 || kind == 10 || kind == 11) {
            auto json = nlohmann::json::parse(Read(manifest));
            if (kind == 8)
                json["container_version"] = 3;
            if (kind == 9)
                json["container"] = "unknown";
            if (kind == 10)
                json["container_version"] = "2";
            if (kind == 11)
                json["container_version"] = 1;
            Write(manifest, json.dump());
        } else if (kind == 12) {
            fs::remove_all(WorldSaveService::region_directory(save));
            Write(save / "chunks/region/unknown.bin", "unknown");
        } else if (kind == 13) {
            fs::rename(region, save / "chunks/region/r.invalid.lmr");
        } else if (kind == 21) {
            auto json = nlohmann::json::parse(Read(manifest));
            json["schema"] = "luminumbra.persistence.world_manifest.v3";
            Write(manifest, json.dump());
        } else if (kind == 18) {
            auto json = nlohmann::json::parse(Read(manifest));
            json["schema"] = "unknown";
            Write(manifest, json.dump());
        } else if (kind == 19) {
            auto json = nlohmann::json::parse(Read(manifest));
            json.erase("container_version");
            Write(manifest, json.dump());
        } else if (kind == 20) {
            fs::remove_all(WorldSaveService::region_directory(save));
            Write(WorldSaveService::region_file_path(save, 0, 0), "");
        } else if (kind >= 22) {
            const auto metadata = save / "world_info.json";
            auto json = nlohmann::json::parse(Read(metadata));
            if (kind == 22) {
                // A world created before its first chunk save still has durable metadata.
                fs::remove_all(save / "chunks");
                json.erase("container_version");
            } else if (kind == 23)
                json.erase("container_version"); // mixed old metadata and current chunks
            else if (kind == 24)
                json["container_version"] = Persistence::kScheduledWeatherMetadataVersion + 1;
            else if (kind == 25)
                json["container_version"] = "2";
            else if (kind == 27)
                json["container_version"] = 1;
            else if (kind == 28)
                json["container_version"] = -1;
            else if (kind == 29)
                json = nlohmann::json::array();
            if (kind >= 30) {
                const std::vector<std::string> invalid = {
                    R"({"simulationTick":-1})",
                    R"({"simulationTick":1.5})",
                    R"({"simulationTick":"1"})",
                    R"({"simulationTick":null})",
                    R"({"simulationTick":true})",
                    R"({"simulationTick":[]})",
                    R"({"simulationTick":{}})",
                    R"({"simulationTick":4611686018427387904})",
                    R"({"simulationTick":18446744073709551616})",
                    R"({"calendar":null})",
                    R"({"calendar":[]})",
                    R"({"calendar":"default"})",
                    R"({"calendar":false})",
                    R"({"calendar":{}})",
                    R"({"calendar":{"dayLengthTicks":36000}})",
                    R"({"calendar":{"daysPerYear":8}})",
                    R"({"calendar":{"dayLengthTicks":0,"daysPerYear":8}})",
                    R"({"calendar":{"dayLengthTicks":-1,"daysPerYear":8}})",
                    R"({"calendar":{"dayLengthTicks":2147483648,"daysPerYear":8}})",
                    R"({"calendar":{"dayLengthTicks":36000.0,"daysPerYear":8}})",
                    R"({"calendar":{"dayLengthTicks":true,"daysPerYear":8}})",
                    R"({"calendar":{"dayLengthTicks":"36000","daysPerYear":8}})",
                    R"({"calendar":{"dayLengthTicks":36000,"daysPerYear":0}})",
                    R"({"calendar":{"dayLengthTicks":36000,"daysPerYear":-1}})",
                    R"({"calendar":{"dayLengthTicks":36000,"daysPerYear":367}})",
                    R"({"calendar":{"dayLengthTicks":36000,"daysPerYear":8.5}})",
                    R"({"calendar":{"dayLengthTicks":36000,"daysPerYear":null}})",
                    R"({"calendar":{"dayLengthTicks":36000,"daysPerYear":false}})",
                    R"({"calendar":{"dayLengthTicks":36000,"daysPerYear":"8"}})",
                    R"({"calendar":{"dayLengthTicks":36000,"daysPerYear":8,"extra":1}})"};
                json.update(nlohmann::json::parse(invalid.at(static_cast<std::size_t>(kind - 30))));
            }
            Write(metadata, kind == 26 ? "{" : json.dump());
        } else if (kind == 14 || kind == 15 || kind == 16 || kind == 17) {
            WorldSaveService::ContainerRecord record;
            record.id = World::FarLodStore::tile_record_id(World::FarLodTier::F2, 0, 0);
            record.lod_level = 2;
            if (kind == 17) {
                const auto count =
                    static_cast<std::size_t>(World::FarLodSamplesPerSide(World::FarLodTier::F2));
                record.payload.assign(22u + count * count * 4u, '\0');
                record.payload[0] = 2;
            } else
                record.payload = std::string("FSD2") +
                                 char(kind == 14   ? 2
                                      : kind == 15 ? 4
                                                   : 3) +
                                 char(0);
            ASSERT_TRUE(WorldSaveService::upsert_container_records(region, {record}));
        }
    }
};

TEST_P(WorldOpenRefusal, RefusesBeforeGenerationAndPreservesAllDiskBytes) {
    const auto [entry, kind] = GetParam();
    std::unique_ptr<GameSession> client;
    if (entry < 2) {
        client = std::make_unique<GameSession>();
        client->SetJobSystem(&jobs);
        client->SetRootPath(RootString());
        if (entry == 1) {
            ASSERT_TRUE(client->CreateWorld("Client", "1337", "default"));
        }
    }
    Damage(kind);
    const auto before = DiskBytes(root);
    const auto catalog = Persistence::EnumerateSavedWorlds(root);
    ASSERT_TRUE(catalog.error.empty());
    const auto inspected = Persistence::InspectSavedWorld(root, "fixture");
    EXPECT_FALSE(inspected.error.empty());
    const bool obsolete = kind <= 4 || kind == 11 || kind == 14 || kind == 17 || kind == 22 ||
                          kind == 23 || kind == 27;
    const bool future = kind == 5 || kind == 8 || kind == 15 || kind == 21 || kind == 24;
    EXPECT_FALSE(WorldSaveService::has_world_save(save));
    WorldStreamingState rejected;
    rejected.get_or_create_chunk(IVec3(2, 0, 2));
    std::vector<std::string> diagnostics;
    EXPECT_FALSE(WorldSaveService{}.load_world(rejected, save, diagnostics));
    EXPECT_TRUE(rejected.empty());
    EXPECT_FALSE(diagnostics.empty());
    EXPECT_FALSE(WorldSaveService{}.save_world(rejected, save));
    std::string error;
    if (entry == 2) {
        Server::ServerWorldRunnerConfig config;
        config.root_path = RootString();
        config.world_id = "fixture";
        config.surface_radius = 0;
        config.collision_radius = 0;
        config.autosave_interval_ticks = 1;
        Server::ServerWorldRunner runner(config);
        EXPECT_FALSE(runner.Boot());
        error = runner.GetBootError();
        EXPECT_EQ(runner.Session(), nullptr);
        runner.RunFixedTicks(2);
        world::WorldStateSaveReport report;
        runner.Shutdown(&report);
        EXPECT_FALSE(report.saved);
    } else {
        GameSession& session = *client;
        if (entry == 0) {
            EXPECT_FALSE(session.LoadWorld("fixture"));
        } else {
            // The client's scenario load path opens an explicit save directory.
            EXPECT_FALSE(session.LoadWorldStateFrom(save));
            EXPECT_TRUE(session.GetWorldSystem()->snapshot_streamed_chunks().empty());
        }
        error = session.GetWorldOpenError();
        EXPECT_EQ(session.GetLastLoadedChunkCount(), 0u);
        EXPECT_FALSE(session.SaveWorld());
        EXPECT_FALSE(session.SaveWorldState());
        EXPECT_FALSE(session.SaveWorldStateTo(save));
        EXPECT_FALSE(session.LoadWorldStateFrom(save));
    }
    ASSERT_FALSE(error.empty());
    if (obsolete) {
        EXPECT_EQ(error, WorldSaveService::kObsoleteWorldMessage);
    } else if (future) {
        EXPECT_NE(error.find("future"), std::string::npos);
    } else {
        EXPECT_NE(error, WorldSaveService::kObsoleteWorldMessage);
        EXPECT_EQ(error.find("future"), std::string::npos);
    }
    client.reset(); // includes client teardown
    EXPECT_EQ(DiskBytes(root), before);
}

INSTANTIATE_TEST_SUITE_P(PersistenceEntryPoints,
                         WorldOpenRefusal,
                         testing::Combine(testing::Range(0, 3), testing::Range(0, 30)));

INSTANTIATE_TEST_SUITE_P(ClockMetadataEntryPoints,
                         WorldOpenRefusal,
                         testing::Combine(testing::Range(0, 3), testing::Range(30, 60)));

TEST(WorldSeedConversion, PinnedUtf8AndNumericCompatibility) {
    EXPECT_EQ(world::ResolveWorldSeed("hello", 1), UINT32_C(614098253));
    EXPECT_EQ(world::ResolveWorldSeed("portable-\xc3\xa9", 1), UINT32_C(3978087486));
    EXPECT_EQ(world::ResolveWorldSeed("18446744073709551616", 1), UINT32_C(3574524678));
    for (const auto& [text, expected] :
         std::vector<std::pair<std::string, std::uint32_t>>{{"0", 0},
                                                            {"1337", 1337},
                                                            {"4294967295", UINT32_MAX},
                                                            {"4294967296", 0},
                                                            {"18446744073709551615", UINT32_MAX},
                                                            {"-1", UINT32_MAX},
                                                            {"  +42tail", 42},
                                                            {"42text", 42}}) {
        SCOPED_TRACE(text);
        EXPECT_EQ(world::ResolveWorldSeed(text, 0), expected);
        EXPECT_EQ(world::ResolveWorldSeed(text, 1), expected);
    }
    EXPECT_THROW(world::ResolveWorldSeed("hello", 2), std::invalid_argument);
}

#if defined(_WIN32)
TEST(WorldSeedConversion, NativeWindowsCommandLinePreservesUtf8AndRefusesInvalidInput) {
    using world::WorldSeedFromWindowsCommandLine;
    struct SeedCase {
        std::wstring command;
        int count;
        int index;
        std::string utf8;
    };
    std::string utf8_limit;
    for (int index = 0; index < 2048; ++index)
        utf8_limit += "\xc3\xa9";
    for (const auto& input : std::vector<SeedCase>{
             {L"server --seed portable-\u00e9", 3, 2, "portable-\xc3\xa9"},
             {L"server --seed \u4e16\u754c-\U0001f30d",
              3,
              2,
              "\xe4\xb8\x96\xe7\x95\x8c-\xf0\x9f\x8c\x8d"},
             {L"\"C:\\Program Files\\server.exe\" --seed \"with space \\\\backslash\"",
              3,
              2,
              "with space \\\\backslash"},
             {L"server --seed \"\"", 3, 2, ""},
             {L"server --seed \"a\"\"\"\"b\"", 3, 2, "a\"\"b"},
             {L"server --seed \"a\"\"b c\"", 3, 2, "a\"b c"},
             {L"server --seed a\\\"b", 3, 2, "a\"b"},
             {L"server --seed a\\\\\"b c\"", 3, 2, "a\\b c"},
             {L"\"C:\\a\"b\\server.exe --seed \"x\"", 3, 2, "x"},
             {L"server --seed 0", 3, 2, "0"},
             {L"server --seed \"  +42tail\"", 3, 2, "  +42tail"},
             {L"server --seed first --seed portable-\u00e9", 5, 2, "first"},
             {L"server --seed first --seed portable-\u00e9", 5, 4, "portable-\xc3\xa9"},
             {L"server --seed " + std::wstring(4096, L'x'), 3, 2, std::string(4096, 'x')},
             {L"server --seed " + std::wstring(2048, L'\u00e9'), 3, 2, utf8_limit}}) {
        const auto converted =
            WorldSeedFromWindowsCommandLine(input.command.c_str(), input.count, input.index);
        ASSERT_TRUE(converted.has_value());
        if (converted) {
            EXPECT_EQ(*converted, input.utf8);
            if (!input.utf8.empty()) {
                EXPECT_EQ(world::ResolveWorldSeed(*converted, 1),
                          world::ResolveWorldSeed(input.utf8, 1));
            }
        }
    }
    const auto pinned = WorldSeedFromWindowsCommandLine(L"server --seed portable-\u00e9", 3, 2);
    ASSERT_TRUE(pinned.has_value());
    if (pinned) {
        EXPECT_EQ(world::ResolveWorldSeed(*pinned, 1), UINT32_C(3978087486));
    }
    EXPECT_FALSE(WorldSeedFromWindowsCommandLine(nullptr, 3, 2).has_value());
    for (const auto& [count, index] :
         std::vector<std::pair<int, int>>{{0, 2}, {3, -1}, {3, 0}, {3, 1}, {3, 3}, {4, 2}}) {
        EXPECT_FALSE(
            WorldSeedFromWindowsCommandLine(L"server --seed value", count, index).has_value());
    }
    for (const std::wstring command :
         {L"", L"server --seed", L"server --root value", L"server --seed value extra"}) {
        EXPECT_FALSE(WorldSeedFromWindowsCommandLine(command.c_str(), 3, 2).has_value());
    }
    for (const auto& seed :
         std::vector<std::wstring>{std::wstring(1, static_cast<wchar_t>(0xd800)),
                                   std::wstring(1, static_cast<wchar_t>(0xdc00)),
                                   std::wstring(1, static_cast<wchar_t>(0xd800)) + L"x",
                                   std::wstring(4097, L'x'),
                                   std::wstring(2049, L'\u00e9'),
                                   std::wstring(32767, L'x')}) {
        const auto command = L"server --seed " + seed;
        EXPECT_FALSE(WorldSeedFromWindowsCommandLine(command.c_str(), 3, 2).has_value());
    }
}
#endif

TEST_F(WorldOpenRefusal, VersionedTextSeedGoldenRoundTripsBothSaveFormats) {
    for (const bool indexed : {false, true}) {
        SCOPED_TRACE(indexed);
        GameSession created;
        created.SetRootPath(RootString());
        created.SetJobSystem(&jobs);
        created.SetActiveRegionsEnabled(indexed);
        ASSERT_TRUE(created.CreateWorld("Text seed", "portable-\xc3\xa9", "default"));
        const auto directory = created.GetWorldSaveDir();
        const auto id = created.GetMetadata().worldId;
        EXPECT_EQ(created.GetMetadata().seedConversionVersion, 1u);
        EXPECT_EQ(created.GetMetadata().resolvedSeed, UINT32_C(3978087486));
        EXPECT_EQ(static_cast<std::uint32_t>(created.GetWorldSystem()->get_seed()),
                  UINT32_C(3978087486));
        ASSERT_TRUE(created.SaveWorld());
        auto metadata = nlohmann::json::parse(Read(directory / "world_info.json"));
        EXPECT_EQ(metadata.at("seedConversionVersion"), 1);
        EXPECT_EQ(metadata.at("resolvedSeed"), UINT32_C(3978087486));
        EXPECT_EQ(metadata.at("container_version"), indexed ? 3 : 2);
        const auto before = DiskBytes(directory);
        const auto inspected = Persistence::InspectSavedWorld(root, id);
        ASSERT_TRUE(inspected.error.empty()) << inspected.error;
        EXPECT_EQ(inspected.metadata.seedConversionVersion, 1u);
        EXPECT_EQ(inspected.metadata.resolvedSeed, UINT32_C(3978087486));
        GameSession reopened;
        reopened.SetRootPath(RootString());
        reopened.SetJobSystem(&jobs);
        reopened.SetActiveRegionsEnabled(indexed);
        ASSERT_TRUE(reopened.LoadWorld(id)) << reopened.GetWorldOpenError();
        EXPECT_EQ(reopened.GetMetadata().seedConversionVersion, 1u);
        EXPECT_EQ(reopened.GetMetadata().resolvedSeed, UINT32_C(3978087486));
        EXPECT_EQ(static_cast<std::uint32_t>(reopened.GetWorldSystem()->get_seed()),
                  UINT32_C(3978087486));
        EXPECT_EQ(DiskBytes(directory), before);
        ASSERT_TRUE(reopened.SaveWorld());
        metadata = nlohmann::json::parse(Read(directory / "world_info.json"));
        EXPECT_EQ(metadata.at("seedConversionVersion"), 1);
        EXPECT_EQ(metadata.at("resolvedSeed"), UINT32_C(3978087486));
        const auto exported = root / (indexed ? "export-indexed" : "export-unindexed");
        ASSERT_TRUE(reopened.SaveWorldStateTo(exported, nullptr, true));
        metadata = nlohmann::json::parse(Read(exported / "world_info.json"));
        EXPECT_EQ(metadata.at("seedConversionVersion"), 1);
        EXPECT_EQ(metadata.at("resolvedSeed"), UINT32_C(3978087486));
    }
}

TEST_F(WorldOpenRefusal, UnmarkedTextSeedReopensAndResavesWithLegacyConversion) {
    auto metadata = nlohmann::json::parse(Read(save / "world_info.json"));
    metadata["seed"] = "portable-\xc3\xa9";
    Write(save / "world_info.json", metadata.dump());
    const auto before = DiskBytes(save);
    GameSession session;
    session.SetRootPath(RootString());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(session.LoadWorld("fixture")) << session.GetWorldOpenError();
    EXPECT_EQ(session.GetMetadata().seedConversionVersion, 0u);
    EXPECT_EQ(static_cast<std::uint32_t>(session.GetWorldSystem()->get_seed()),
              static_cast<std::uint32_t>(std::hash<std::string>{}("portable-\xc3\xa9")));
    EXPECT_EQ(DiskBytes(save), before);
    ASSERT_TRUE(session.SaveWorld());
    metadata = nlohmann::json::parse(Read(save / "world_info.json"));
    EXPECT_FALSE(metadata.contains("seedConversionVersion"));
    EXPECT_FALSE(metadata.contains("resolvedSeed"));
    ASSERT_TRUE(session.LoadWorld("fixture")) << session.GetWorldOpenError();
    EXPECT_EQ(static_cast<std::uint32_t>(session.GetWorldSystem()->get_seed()),
              static_cast<std::uint32_t>(std::hash<std::string>{}("portable-\xc3\xa9")));
}

TEST_F(WorldOpenRefusal, NumericAndEmptyNewSeedsKeepTheirExistingBehavior) {
    const std::array<std::string, 4> inputs = {"0", "4294967296", "42text", ""};
    for (const std::string& text : inputs) {
        SCOPED_TRACE(text);
        GameSession session;
        session.SetRootPath(RootString());
        session.SetJobSystem(&jobs);
        session.SetActiveRegionsEnabled(false);
        ASSERT_TRUE(session.CreateWorld("Numeric seed", text, "default"));
        const auto& metadata = session.GetMetadata();
        ASSERT_FALSE(metadata.seed.empty());
        const auto expected = static_cast<std::uint32_t>(std::stoull(metadata.seed));
        EXPECT_EQ(metadata.resolvedSeed, expected);
        EXPECT_EQ(static_cast<std::uint32_t>(session.GetWorldSystem()->get_seed()), expected);
        EXPECT_EQ(metadata.seedConversionVersion, 1u);
        if (!text.empty()) {
            EXPECT_EQ(metadata.seed, text);
        }
        ASSERT_TRUE(session.LoadWorld(metadata.worldId)) << session.GetWorldOpenError();
        EXPECT_EQ(static_cast<std::uint32_t>(session.GetWorldSystem()->get_seed()), expected);
    }
}

class ReplaySeedCompatibility : public WorldOpenRefusal {};

TEST_P(ReplaySeedCompatibility, RecordedSeedReproducesTheStartingWorld) {
    const auto [seed_case, new_recording] = GetParam();
    const std::array<std::string, 15> seeds = {"portable-\xc3\xa9",
                                               "18446744073709551616",
                                               "0",
                                               "4294967295",
                                               "4294967296",
                                               "4294967296tail",
                                               "8589934591",
                                               "8589934592",
                                               "18446744073709551615",
                                               "-1",
                                               "  +42tail",
                                               "42text",
                                               "+",
                                               " ",
                                               ""};
    ServerCliOptions options;
    options.root = RootString();
    options.seed = seeds.at(static_cast<std::size_t>(seed_case));
    options.surface_radius = 0;
    options.collision_radius = 0;
    options.test_streaming_radius_cap = 1;
    options.ticks = 0;
    options.systems_config_override = luminumbra::core::SystemConfig::FromJsonString(
        R"({"sim":{"active_regions":{"enabled":false}}})");
    options.record_path = Filesystem::GenericUtf8PathString(root / "seed.lrec");
    std::uint32_t expected_seed = 0;
    if (new_recording != 0) {
        ASSERT_EQ(RunRecord(options), 0);
    } else {
        // Empty legacy headers discarded the random seed; retain their boot policy.
        if (!options.seed.empty()) {
            try {
                expected_seed = static_cast<std::uint32_t>(std::stoull(options.seed));
            } catch (const std::exception&) {
                expected_seed = static_cast<std::uint32_t>(std::hash<std::string>{}(options.seed));
            }
        }
        auto reference_config = RunnerConfigFrom(options);
        reference_config.seed = std::to_string(expected_seed);
        reference_config.autosave_interval_ticks = 0;
        Server::ServerWorldRunner reference(std::move(reference_config));
        ASSERT_TRUE(reference.Boot()) << reference.GetBootError();
        Replay::ReplayHeader header;
        header.seed_string = options.seed;
        header.seed = std::strtoull(options.seed.c_str(), nullptr, 10);
        header.preset = options.preset;
        header.surface_radius = 0;
        header.collision_radius = 0;
        header.engine_version = std::string(luminumbra::core::GetEngineVersionString());
        header.system_config_identity = reference.Session()->SystemConfigIdentity();
        header.preset_content_hash = reference.Session()->PresetContentHash();
        header.start_world_hash = reference.ComputeWorldHash();
        Replay::ReplayWriter writer;
        ASSERT_TRUE(writer.Open(options.record_path, header));
        ASSERT_TRUE(writer.Finalize(0));
    }
    const auto recording = Replay::ReadReplay(options.record_path);
    ASSERT_TRUE(recording.has_value());
    if (!recording)
        return;
    const auto& header = recording->header;
    EXPECT_EQ(header.version,
              new_recording != 0 ? Replay::kLrec1BootVersion : Replay::kLrec1Version);
    EXPECT_EQ(header.authority_boot.has_value(), new_recording != 0);
    EXPECT_EQ(header.seed_string, options.seed);
    if (new_recording != 0) {
        ASSERT_TRUE(header.authority_boot.has_value());
        if (!header.authority_boot.has_value())
            return;
        EXPECT_EQ(header.authority_boot->system_config_identity, header.system_config_identity);
        EXPECT_EQ(header.authority_boot->resolved_world_seed,
                  static_cast<std::uint32_t>(header.seed));
        std::vector<std::uint8_t> encoded;
        std::string error;
        ASSERT_TRUE(world::EncodeAuthorityBootDescriptor(*header.authority_boot, encoded, error))
            << error;
        world::AuthorityBootDescriptor decoded;
        ASSERT_TRUE(world::DecodeAuthorityBootDescriptor(encoded, decoded, error)) << error;
        EXPECT_EQ(decoded, *header.authority_boot);
        EXPECT_EQ(header.seed >> 32, 1u);
        expected_seed = static_cast<std::uint32_t>(header.seed);
        if (!options.seed.empty()) {
            EXPECT_EQ(expected_seed, world::ResolveWorldSeed(options.seed, 1));
        }
    }
    const auto config = ReplayRunnerConfigFrom(options, header);
    ASSERT_TRUE(config.has_value());
    if (!config)
        return;
    if (new_recording == 0 && options.seed.empty()) {
        EXPECT_TRUE(config->seed.empty());
    } else {
        EXPECT_EQ(world::ResolveWorldSeed(config->seed, 1), expected_seed);
        Server::ServerWorldRunner playback(*config, {.owner_continuation = new_recording != 0});
        ASSERT_TRUE(playback.Boot()) << playback.GetBootError();
        if (new_recording != 0) {
            EXPECT_EQ(playback.CaptureReplayHeader().authority_boot, header.authority_boot);
        }
        EXPECT_EQ(static_cast<std::uint32_t>(playback.Session()->GetWorldSystem()->get_seed()),
                  expected_seed);
        EXPECT_EQ(playback.ComputeWorldHash(), header.start_world_hash);
    }
}

INSTANTIATE_TEST_SUITE_P(LegacyAndResolvedHeaders,
                         ReplaySeedCompatibility,
                         testing::Combine(testing::Range(0, 15), testing::Values(0, 1)));

TEST_F(WorldOpenRefusal, ReplaySeedTagBoundariesAndUnknownTagsKeepLegacyFallback) {
    ServerCliOptions options;
    options.root = RootString();
    const auto hello = RuntimeHelloFrom(options);
    Replay::ReplayHeader header;
    header.seed_string = "portable-\xc3\xa9";
    header.preset = options.preset;
    header.surface_radius = 0;
    header.collision_radius = 0;
    header.system_config_identity = hello.system_config_identity;
    header.preset_content_hash = hello.preset_content_hash;
    const auto legacy_seed =
        static_cast<std::uint32_t>(std::hash<std::string>{}(header.seed_string));
    for (const auto version : {Replay::kLrec1Version, Replay::kLrec1ElidedVersion}) {
        header.version = version;
        for (const auto seed : {UINT64_C(0),
                                Replay::kResolvedSeedTag - 1,
                                Replay::kResolvedSeedTag,
                                (Replay::kResolvedSeedTag << 1) - 1,
                                Replay::kResolvedSeedTag << 1,
                                UINT64_MAX}) {
            SCOPED_TRACE(seed);
            header.seed = seed;
            const auto config = ReplayRunnerConfigFrom(options, header);
            ASSERT_TRUE(config.has_value());
            if (config) {
                const bool tagged =
                    seed >= Replay::kResolvedSeedTag && seed < (Replay::kResolvedSeedTag << 1);
                EXPECT_EQ(world::ResolveWorldSeed(config->seed, 1),
                          tagged ? static_cast<std::uint32_t>(seed) : legacy_seed);
            }
        }
        for (const auto seed : {UINT64_C(0), Replay::kResolvedSeedTag}) {
            header.seed = seed;
            header.seed_string.assign(Persistence::SavedInputLimits::kWorldSeedBytes, 'x');
            EXPECT_TRUE(ReplayRunnerConfigFrom(options, header).has_value());
            header.seed_string.push_back('x');
            const auto before = DiskBytes(root);
            EXPECT_FALSE(ReplayRunnerConfigFrom(options, header).has_value());
            EXPECT_EQ(DiskBytes(root), before);
        }
        header.seed_string = "portable-\xc3\xa9";
    }
}

TEST_F(WorldOpenRefusal, SyntheticNumericSeedsRemainCompatibleWithoutConversionMetadata) {
    Persistence::SyntheticSaveSpec spec;
    spec.seed = UINT64_MAX;
    spec.chunk_count = 1;
    spec.world_id = "versioned-synthetic";
    spec.preset_source = root / "worlds/atlas/presets/default.json";
    const auto report = Persistence::WriteSyntheticSave(spec, root);
    ASSERT_TRUE(report.ok) << report.error;
    const auto metadata = nlohmann::json::parse(Read(report.save_dir / "world_info.json"));
    EXPECT_FALSE(metadata.contains("seedConversionVersion"));
    EXPECT_FALSE(metadata.contains("resolvedSeed"));
    EXPECT_EQ(world::ResolveWorldSeed(metadata.at("seed").get<std::string>(), 0), UINT32_MAX);
    const auto inspected = Persistence::InspectSavedWorld(root, spec.world_id);
    ASSERT_TRUE(inspected.error.empty()) << inspected.error;
    EXPECT_EQ(inspected.metadata.seedConversionVersion, 0u);
    EXPECT_EQ(world::ResolveWorldSeed(inspected.metadata.seed, 0), UINT32_MAX);
}

class SeedMetadataRefusal : public WorldOpenRefusal {};

TEST_P(SeedMetadataRefusal, RefusesEveryReaderAndWriterWithoutPublishing) {
    const auto [metadata_only, defect] = GetParam();
    GameSession session;
    session.SetRootPath(RootString());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(session.LoadWorld("fixture"));
    const auto* previous_world = session.GetWorldSystem();
    const auto previous_seed = previous_world->get_seed();
    auto metadata = nlohmann::json::parse(Read(save / "world_info.json"));
    metadata["seed"] = "hello";
    metadata["seedConversionVersion"] = 1;
    metadata["resolvedSeed"] = UINT32_C(614098253);
    if (defect == 0)
        metadata["resolvedSeed"] = UINT32_C(614098254);
    else if (defect == 1)
        metadata["seedConversionVersion"] = 2;
    else if (defect == 2)
        metadata.erase("resolvedSeed");
    else if (defect == 3)
        metadata.erase("seedConversionVersion");
    else if (defect < 11) {
        const std::array<nlohmann::json, 7> invalid = {0, -1, 1.0, true, "1", nullptr, UINT64_MAX};
        metadata["seedConversionVersion"] = invalid.at(static_cast<std::size_t>(defect - 4));
    } else if (defect < 18) {
        const std::array<nlohmann::json, 7> invalid = {
            -1, UINT64_C(4294967296), 614098253.0, true, "614098253", nullptr, 1e38};
        metadata["resolvedSeed"] = invalid.at(static_cast<std::size_t>(defect - 11));
    } else if (defect == 18)
        metadata.erase("seed");
    else if (defect == 19)
        metadata["seed"] = "";
    else if (defect == 20)
        metadata["seed"] = false;
    else if (defect == 21)
        metadata["seed"] = "other text";
    else
        metadata["seedConversionVersion"] = 1e38;
    const auto bytes = metadata.dump();
    const auto writer_destination = root / "refused-destination";
    const auto before_write = DiskBytes(root);
    std::vector<std::string> errors;
    EXPECT_FALSE(WorldSaveService::admit_metadata_bytes(bytes, &errors));
    ASSERT_FALSE(errors.empty());
    EXPECT_FALSE(WorldSaveService::save_metadata(bytes, writer_destination));
    world::ActiveRegionLedger ledger;
    EXPECT_FALSE(
        WorldSaveService::save_metadata_and_active_regions(bytes, ledger, writer_destination));
    const world::GroundObjectStore objects;
    EXPECT_FALSE(WorldSaveService::save_metadata_active_regions_and_ground_objects(
        bytes, ledger, objects, {}, writer_destination));
    EXPECT_EQ(DiskBytes(root), before_write);
    Write(save / "world_info.json", bytes);
    if (metadata_only != 0)
        fs::remove_all(save / "chunks");
    const auto before_read = DiskBytes(root);
    EXPECT_FALSE(WorldSaveService::validate_save(save));
    WorldSaveService::SaveFormat format;
    EXPECT_FALSE(WorldSaveService::read_save_format(save, format));
    world::WorldClock clock;
    bool requires_regions = false;
    EXPECT_FALSE(WorldSaveService::read_clock_metadata(save, clock, requires_regions));
    Persistence::MemberDigest digest;
    std::string error;
    EXPECT_FALSE(WorldSaveService::member_digest(save, "world_info.json", digest, error));
    const auto inspected = Persistence::InspectSavedWorld(root, "fixture");
    EXPECT_FALSE(inspected.error.empty());
    const auto catalog = Persistence::EnumerateSavedWorlds(root);
    ASSERT_EQ(catalog.worlds.size(), 1u);
    EXPECT_FALSE(catalog.worlds.front().error.empty());
    EXPECT_FALSE(session.LoadWorld("fixture"));
    EXPECT_EQ(session.GetWorldSystem(), previous_world);
    EXPECT_EQ(session.GetWorldSystem()->get_seed(), previous_seed);
    EXPECT_EQ(session.GetMetadata().seed, "1337");
    EXPECT_FALSE(session.LoadWorldStateFrom(save));
    Server::ServerWorldRunnerConfig config;
    config.root_path = RootString();
    config.world_id = "fixture";
    config.surface_radius = 0;
    config.collision_radius = 0;
    config.autosave_interval_ticks = 0;
    Server::ServerWorldRunner runner(config);
    EXPECT_FALSE(runner.Boot());
    EXPECT_EQ(runner.Session(), nullptr);
    runner.Shutdown();
    EXPECT_EQ(DiskBytes(root), before_read);
}

INSTANTIATE_TEST_SUITE_P(InitializedAndMetadataOnly,
                         SeedMetadataRefusal,
                         testing::Combine(testing::Values(0, 1), testing::Range(0, 23)));

TEST_F(WorldOpenRefusal, MissingMetadataRefusesCatalogClientAndServerWithoutChangingBytes) {
    ASSERT_TRUE(fs::remove(save / "world_info.json"));
    const auto before = DiskBytes(root);
    const std::string expected =
        "World unavailable: its save directory or metadata is missing or unreadable.";
    const auto inspected = Persistence::InspectSavedWorld(root, "fixture");
    EXPECT_EQ(inspected.error, expected);
    const auto catalog = Persistence::EnumerateSavedWorlds(root);
    ASSERT_TRUE(catalog.error.empty());
    ASSERT_EQ(catalog.worlds.size(), 1u);
    EXPECT_EQ(catalog.worlds.front().error, expected);
    {
        GameSession client;
        client.SetJobSystem(&jobs);
        client.SetRootPath(RootString());
        EXPECT_FALSE(client.LoadWorld("fixture"));
        EXPECT_EQ(client.GetWorldOpenError(), expected);
        EXPECT_EQ(client.GetLastLoadedChunkCount(), 0u);
        EXPECT_FALSE(client.SaveWorld());
    }
    Server::ServerWorldRunnerConfig config;
    config.root_path = RootString();
    config.world_id = "fixture";
    config.surface_radius = 0;
    config.collision_radius = 0;
    Server::ServerWorldRunner runner(config);
    EXPECT_FALSE(runner.Boot());
    EXPECT_EQ(runner.GetBootError(), expected);
    EXPECT_EQ(runner.Session(), nullptr);
    world::WorldStateSaveReport report;
    runner.Shutdown(&report);
    EXPECT_FALSE(report.saved);
    EXPECT_EQ(DiskBytes(root), before);
}

TEST_F(WorldOpenRefusal, SeparateSaveRootUsesRuntimeFallbackForLegacyPresets) {
    const auto storage = root / "storage";
    fs::create_directories(storage / "worlds");
    fs::rename(root / "worlds/saves", storage / "worlds/saves");
    save = storage / "worlds/saves/fixture";
    ASSERT_FALSE(fs::exists(save / "preset.json"));
    for (const std::string name : {"default", "legacy_custom"}) {
        SCOPED_TRACE(name);
        const auto preset =
            root / (name == "default" ? "worlds/atlas/presets-rev6" : "worlds/atlas/presets") /
            (name + ".json");
        if (name != "default")
            fs::copy_file(root / "worlds/atlas/presets/default.json", preset);
        auto metadata = nlohmann::json::parse(Read(save / "world_info.json"));
        metadata["worldType"] = name;
        Write(save / "world_info.json", metadata.dump());
        const auto inspected = Persistence::InspectSavedWorld(storage, root, "fixture");
        ASSERT_TRUE(inspected.error.empty()) << inspected.error;
        EXPECT_EQ(inspected.preset_path, preset);
        const auto catalog = Persistence::EnumerateSavedWorlds(storage, root);
        ASSERT_TRUE(catalog.error.empty()) << catalog.error;
        ASSERT_EQ(catalog.worlds.size(), 1u);
        EXPECT_TRUE(catalog.worlds.front().error.empty());
        EXPECT_EQ(catalog.worlds.front().preset_path, preset);
        GameSession session;
        session.SetRootPath(RootString());
        session.SetSaveRootPath(storage);
        session.SetJobSystem(&jobs);
        ASSERT_TRUE(session.LoadWorld("fixture")) << session.GetWorldOpenError();
        EXPECT_EQ(session.GetMetadata().worldType, name);
    }
}

TEST_F(WorldOpenRefusal, SeparateSaveRootRetainsEmbeddedPresetRefusals) {
    const auto storage = root / "storage";
    fs::create_directories(storage / "worlds");
    fs::rename(root / "worlds/saves", storage / "worlds/saves");
    save = storage / "worlds/saves/fixture";
    const auto canonical =
        nlohmann::json::parse(Read(root / "worlds/atlas/presets/default.json")).dump();
    const auto original = nlohmann::json::parse(Read(save / "world_info.json"));
    for (const std::string name : {"default", "custom"}) {
        SCOPED_TRACE(name);
        Write(root / "worlds/atlas/presets" / (name + ".json"), canonical);
        auto metadata = original;
        metadata["worldType"] = name;
        metadata["presetFormat"] = Persistence::kSavedPresetFormat;
        metadata["presetIdentity"] = Persistence::StableChecksum(canonical);
        Write(save / "preset.json", canonical);
        Write(save / "world_info.json", metadata.dump());
        const auto valid = Persistence::InspectSavedWorld(storage, root, "fixture");
        ASSERT_TRUE(valid.error.empty()) << valid.error;
        EXPECT_EQ(valid.preset_path, save / "preset.json");
        EXPECT_EQ(valid.preset_json, canonical);
        for (const std::string defect :
             {"missing preset", "missing identity", "identity mismatch", "unsupported format"}) {
            SCOPED_TRACE(defect);
            Write(save / "preset.json", canonical);
            auto damaged = metadata;
            if (defect == "missing preset")
                fs::remove(save / "preset.json");
            else if (defect == "missing identity")
                damaged.erase("presetIdentity");
            else if (defect == "identity mismatch")
                damaged["presetIdentity"] = "mismatched";
            else
                damaged["presetFormat"] = 0;
            Write(save / "world_info.json", damaged.dump());
            const auto before = DiskBytes(root);
            const auto inspected = Persistence::InspectSavedWorld(storage, root, "fixture");
            ASSERT_FALSE(inspected.error.empty());
            const auto catalog = Persistence::EnumerateSavedWorlds(storage, root);
            ASSERT_TRUE(catalog.error.empty()) << catalog.error;
            ASSERT_EQ(catalog.worlds.size(), 1u);
            EXPECT_EQ(catalog.worlds.front().error, inspected.error);
            GameSession session;
            session.SetRootPath(RootString());
            session.SetSaveRootPath(storage);
            session.SetJobSystem(&jobs);
            EXPECT_FALSE(session.LoadWorld("fixture"));
            EXPECT_EQ(session.GetWorldOpenError(), inspected.error);
            EXPECT_EQ(session.GetWorldSystem(), nullptr);
            EXPECT_FALSE(session.SaveWorld());
            EXPECT_FALSE(session.SaveWorldState());
            EXPECT_EQ(DiskBytes(root), before);
        }
    }
}

TEST_F(WorldOpenRefusal, MissingEmbeddedPresetAndAtlasFallbackRefuseBeforeGeneration) {
    const auto atlas = root / "worlds/atlas/presets-rev6/default.json";
    const auto embedded = save / "preset.json";
    ASSERT_TRUE(fs::copy_file(atlas, embedded));
    ASSERT_TRUE(Persistence::InspectSavedWorld(root, "fixture").error.empty());
    ASSERT_TRUE(fs::remove(embedded));
    // An embedded-preset deletion alone is loadable through the frozen revision-6 atlas fallback.
    ASSERT_TRUE(Persistence::InspectSavedWorld(root, "fixture").error.empty());
    EXPECT_EQ(Persistence::InspectSavedWorld(root, "fixture").preset_path, atlas);
    ASSERT_TRUE(fs::remove(atlas));
    ASSERT_TRUE(fs::remove(root / "worlds/atlas/presets/default.json"));
    const auto before = DiskBytes(root);
    ExpectMissingPresetReason(Persistence::InspectSavedWorld(root, "fixture").error, atlas);
    const auto catalog = Persistence::EnumerateSavedWorlds(root);
    ASSERT_TRUE(catalog.error.empty());
    ASSERT_EQ(catalog.worlds.size(), 1u);
    ExpectMissingPresetReason(catalog.worlds.front().error, atlas);
    {
        GameSession client;
        client.SetJobSystem(&jobs);
        client.SetRootPath(RootString());
        EXPECT_FALSE(client.LoadWorld("fixture"));
        ExpectMissingPresetReason(client.GetWorldOpenError(), atlas);
        EXPECT_EQ(client.GetWorldSystem(), nullptr);
        EXPECT_EQ(client.GetLastLoadedChunkCount(), 0u);
        EXPECT_FALSE(client.SaveWorld());
    }
    Server::ServerWorldRunnerConfig config;
    config.root_path = RootString();
    config.world_id = "fixture";
    config.surface_radius = 0;
    config.collision_radius = 0;
    Server::ServerWorldRunner runner(config);
    EXPECT_FALSE(runner.Boot());
    ExpectMissingPresetReason(runner.GetBootError(), atlas);
    EXPECT_EQ(runner.Session(), nullptr);
    world::WorldStateSaveReport report;
    runner.Shutdown(&report);
    EXPECT_FALSE(report.saved);
    EXPECT_EQ(DiskBytes(root), before);
}

TEST_F(WorldOpenRefusal, ActiveClockSaveRefusesWithFeatureOffThroughEveryOpenPath) {
    // This case exercises the legacy clock path, independently of shipping defaults.
    Write(root / "data/common/systems.json", R"({"sim":{"active_regions":{"enabled":false}}})");
    GameSession writer;
    writer.SetRootPath(RootString());
    writer.SetJobSystem(&jobs);
    writer.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(writer.LoadWorld("fixture"));
    writer.TickSimulation(writer.GetSimulationClock().fixed_dt());
    ASSERT_TRUE(writer.SaveWorldState());
    const auto before = DiskBytes(root);
    const auto inspected = Persistence::InspectSavedWorld(root, "fixture");
    ASSERT_TRUE(inspected.error.empty());
    EXPECT_TRUE(inspected.requires_active_regions);
    EXPECT_EQ(inspected.clock.tick(), 1u);
    GameSession reader;
    reader.SetActiveRegionsEnabled(false); // Required for the feature-off refusal.
    reader.SetRootPath(RootString());
    reader.SetJobSystem(&jobs);
    ASSERT_TRUE(reader.CreateTransientWorld("Reader", "1337", "default"));
    EXPECT_FALSE(reader.LoadWorldStateFrom(save));
    EXPECT_NE(reader.GetWorldOpenError().find("Incompatible configuration"), std::string::npos);
    EXPECT_FALSE(reader.SaveWorldStateTo(save));
    EXPECT_FALSE(reader.LoadWorld("fixture"));
    EXPECT_NE(reader.GetWorldOpenError().find("Incompatible configuration"), std::string::npos);
    EXPECT_FALSE(reader.SaveWorld());
    Server::ServerWorldRunnerConfig config;
    config.root_path = RootString();
    config.world_id = "fixture";
    config.surface_radius = 0;
    config.collision_radius = 0;
    Server::ServerWorldRunner runner(config);
    EXPECT_FALSE(runner.Boot());
    EXPECT_NE(runner.GetBootError().find("Incompatible configuration"), std::string::npos);
    runner.Shutdown();
    EXPECT_EQ(DiskBytes(root), before);
}

TEST_F(WorldOpenRefusal, CorruptClockMetadataOnlySaveRefusesBeforeFirstChunkSave) {
    fs::remove_all(save / "chunks");
    auto metadata = nlohmann::json::parse(Read(save / "world_info.json"));
    metadata["simulationTick"] = -1;
    Write(save / "world_info.json", metadata.dump());
    const auto before = DiskBytes(root);
    GameSession session;
    session.SetRootPath(RootString());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    EXPECT_FALSE(session.LoadWorld("fixture"));
    EXPECT_NE(session.GetWorldOpenError().find("simulationTick"), std::string::npos);
    EXPECT_FALSE(session.SaveWorld());
    EXPECT_FALSE(WorldSaveService::validate_save(save));
    EXPECT_EQ(DiskBytes(root), before);
}

TEST_F(WorldOpenRefusal, CalendarAndMarkerRefusalMatrixPreservesInitializedAndMetadataOnlySaves) {
    const auto base = nlohmann::json::parse(Read(save / "world_info.json"));
    for (const bool metadata_only : {false, true}) {
        if (metadata_only)
            fs::remove_all(save / "chunks");
        for (const auto& patch : TestSupport::InvalidCalendarMetadata()) {
            SCOPED_TRACE(patch.dump());
            auto metadata = base;
            metadata.update(patch);
            Write(save / "world_info.json", metadata.dump());
            const auto before = DiskBytes(save);
            EXPECT_FALSE(Persistence::InspectSavedWorld(root, "fixture").error.empty());
            const auto catalog = Persistence::EnumerateSavedWorlds(root);
            ASSERT_EQ(catalog.worlds.size(), 1u);
            EXPECT_FALSE(catalog.worlds.front().error.empty());
            EXPECT_FALSE(WorldSaveService::validate_save(save));
            GameSession session;
            session.SetJobSystem(&jobs);
            session.SetRootPath(RootString());
            session.SetActiveRegionsEnabled(true);
            EXPECT_FALSE(session.LoadWorld("fixture"));
            GameSession snapshot;
            snapshot.SetJobSystem(&jobs);
            snapshot.SetRootPath(RootString());
            ASSERT_TRUE(snapshot.CreateTransientWorld("Snapshot reader", "1337", "default"));
            EXPECT_FALSE(snapshot.LoadWorldStateFrom(save));
            EXPECT_FALSE(snapshot.SaveWorldStateTo(save));
            EXPECT_EQ(DiskBytes(save), before);
        }
    }
}

TEST_F(WorldOpenRefusal, SupportedCalendarFormsAreReadOnlyAndMarkerDoesNotRequireFeature) {
    const auto base = nlohmann::json::parse(Read(save / "world_info.json"));
    fs::remove_all(save / "chunks");
    const auto v2 = TestSupport::DefaultCalendarMetadata();
    const auto v1 = nlohmann::json{{"dayLengthTicks", 36000}, {"daysPerYear", 3}};
    for (const auto& patch : {nlohmann::json::object(),
                              nlohmann::json{{"calendar", v1}},
                              nlohmann::json{{"calendar", v2}},
                              nlohmann::json{{"renderCalendar", v2}}}) {
        fs::remove_all(save / "chunks");
        auto metadata = base;
        metadata.update(patch);
        Write(save / "world_info.json", metadata.dump());
        const auto before = DiskBytes(save);
        const auto inspected = Persistence::InspectSavedWorld(root, "fixture");
        ASSERT_TRUE(inspected.error.empty()) << inspected.error;
        EXPECT_EQ(inspected.requires_active_regions, patch.contains("calendar"));
        EXPECT_EQ(inspected.render_calendar.has_value(), patch.contains("renderCalendar"));
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(RootString());
        session.SetActiveRegionsEnabled(patch.contains("calendar"));
        ASSERT_TRUE(session.LoadWorld("fixture")) << session.GetWorldOpenError();
        const auto expected =
            patch.contains("renderCalendar") ||
                    patch.value("calendar", nlohmann::json::object()).contains("version")
                ? world::WorldCalendar{}
            : patch.contains("calendar") ? world::WorldCalendar{36000, 3, 1, 2, 8}
                                         : world::WorldCalendar::Legacy();
        EXPECT_EQ(session.GetRenderCalendar(), expected);
        EXPECT_EQ(DiskBytes(save), before);
        ASSERT_TRUE(session.SaveWorld());
        const auto written = nlohmann::json::parse(Read(save / "world_info.json"));
        if (patch.contains("calendar")) {
            EXPECT_EQ(written.at("calendar"), patch.at("calendar"));
        } else {
            EXPECT_EQ(written.contains("renderCalendar"), patch.contains("renderCalendar"));
        }
    }
}

TEST_F(WorldOpenRefusal, CurrentMetadataOnlyWorldLoadsBeforeFirstChunkSave) {
    fs::remove_all(save / "chunks");
    const auto before = DiskBytes(save);
    EXPECT_FALSE(WorldSaveService::has_world_save(save));
    EXPECT_TRUE(WorldSaveService::validate_save(save));
    GameSession session;
    session.SetJobSystem(&jobs);
    session.SetRootPath(RootString());
    ASSERT_TRUE(session.LoadWorld("fixture")) << session.GetWorldOpenError();
    EXPECT_EQ(session.GetLastLoadedChunkCount(), 0u);
    EXPECT_EQ(DiskBytes(save), before);
}

TEST_F(WorldOpenRefusal, CurrentClientLoadPreservesAuthoritativeChunkAndFreshMissSucceeds) {
    GameSession session;
    session.SetJobSystem(&jobs);
    session.SetRootPath(RootString());
    ASSERT_TRUE(session.LoadWorld("fixture")) << session.GetWorldOpenError();
    EXPECT_EQ(session.GetLastLoadedChunkCount(), 1u);
    auto chunks = session.GetWorldSystem()->snapshot_streamed_chunks();
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks.front()->sdf_provenance(), ChunkSdfProvenance::LoadedOrEdited);
    EXPECT_EQ(chunks.front()->sdf_data.front(), 1.0f);
    ASSERT_TRUE(session.CreateWorld("Fresh", "1337", "default"));
    EXPECT_TRUE(session.LoadWorldState());
    EXPECT_TRUE(session.GetWorldOpenError().empty());
    EXPECT_EQ(session.GetLastLoadedChunkCount(), 0u);
}
TEST_F(WorldOpenRefusal, CurrentServerBootKeepsLoadedAuthorityAndSkipsWaterSettle) {
    // Keep the legacy clock and the fixture's 8x8 water grids: this test checks
    // loaded authority/no-settle, independently of clock and resolution migrations.
    Write(root / "data/common/systems.json",
          R"({"sim":{"active_regions":{"enabled":false},"water_high_res":{"enabled":false}}})");
    Server::ServerWorldRunnerConfig config;
    config.root_path = RootString();
    config.world_id = "fixture";
    config.surface_radius = 0;
    config.collision_radius = 0;
    auto expected = DiskBytes(save);
    auto metadata = nlohmann::json::parse(expected.at("world_info.json"));
    // Existing metadata is reconciled even at zero; absent metadata stays absent.
    metadata["waterSimCursor"] = 0;
    expected["world_info.json"] = metadata.dump(4) + "\n";
    Server::ServerWorldRunner runner(config);
    ASSERT_TRUE(runner.Boot()) << runner.GetBootError();
    ASSERT_NE(runner.Session(), nullptr);
    EXPECT_EQ(runner.Session()->GetLastLoadedChunkCount(), 1u);
    EXPECT_TRUE(runner.GetBootSettleStats().water_settle_skipped);
    bool found = false;
    for (const auto& chunk : runner.Session()->GetWorldSystem()->snapshot_streamed_chunks()) {
        if (chunk->get_coords() == IVec3(0, 0, 0)) {
            found = true;
            EXPECT_EQ(chunk->sdf_provenance(), ChunkSdfProvenance::LoadedOrEdited);
            EXPECT_EQ(chunk->sdf_data.front(), 1.0f);
        }
    }
    EXPECT_TRUE(found);
    runner.Shutdown();
    EXPECT_EQ(DiskBytes(save), expected);
}

TEST_F(WorldOpenRefusal, ServerWaterCursorBindsEveryHashCaptureAndSurvivesReload) {
    Write(root / "data/common/systems.json",
          R"({"sim":{"active_regions":{"enabled":false},"water_high_res":{"enabled":false}}})");
    Server::ServerWorldRunnerConfig config;
    config.root_path = RootString();
    config.world_id = "fixture";
    config.surface_radius = 0;
    config.collision_radius = 0;
    // Cap ongoing boot streaming too: the initial horizon radii alone still
    // allow thousands of chunks, each serialized by all three capture APIs.
    config.test_streaming_radius_cap = 1;
    Server::ServerWorldRunner runner(config);
    ASSERT_TRUE(runner.Boot()) << runner.GetBootError();
    auto* world_system = runner.Session()->GetWorldSystem();
    world_system->SetWaterSimWindowCursor(0);
    const auto legacy_hash = runner.ComputeWorldHash();
    const auto legacy_sub = runner.ComputeWorldSubHashes();
    std::string saved_hash;
    Persistence::WorldStreamingStateSubHashes saved_sub;
    for (const std::size_t cursor : {std::size_t{0}, std::size_t{65}, std::size_t{66}}) {
        SCOPED_TRACE(cursor);
        world_system->SetWaterSimWindowCursor(cursor);
        runner.ComputeWorldHashAndSubHashes(saved_hash, saved_sub);
        const auto separate_hash = cursor == 0 ? legacy_hash : runner.ComputeWorldHash();
        const auto separate_sub = cursor == 0 ? legacy_sub : runner.ComputeWorldSubHashes();
        EXPECT_EQ(saved_hash, separate_hash);
        EXPECT_EQ(saved_sub.water, separate_sub.water);
        EXPECT_EQ(saved_sub.terrain, legacy_sub.terrain);
        EXPECT_EQ(saved_sub.entities, legacy_sub.entities);
        if (cursor == 0) {
            EXPECT_EQ(saved_hash, legacy_hash);
            EXPECT_EQ(saved_sub.water, legacy_sub.water);
        } else {
            EXPECT_NE(saved_hash, legacy_hash);
            EXPECT_NE(saved_sub.water, legacy_sub.water);
            EXPECT_EQ(WaterHashDifferenceSection(saved_sub.water, legacy_sub.water),
                      "water/cursor");
        }
    }
    ASSERT_GT(runner.SaveFullSnapshot(), 0u);
    runner.Shutdown();
    Server::ServerWorldRunner restored(config);
    ASSERT_TRUE(restored.Boot()) << restored.GetBootError();
    auto* restored_world = restored.Session()->GetWorldSystem();
    EXPECT_EQ(restored_world->GetWaterSimWindowCursor(), 66u);
    std::string restored_hash;
    Persistence::WorldStreamingStateSubHashes restored_sub;
    restored.ComputeWorldHashAndSubHashes(restored_hash, restored_sub);
    EXPECT_EQ(restored_sub.water, saved_sub.water);
    EXPECT_EQ(restored_sub.entities, saved_sub.entities);
    EXPECT_EQ(restored_sub.wind, saved_sub.wind);
    EXPECT_EQ(restored_sub.weather, saved_sub.weather);
    EXPECT_EQ(restored_sub.aether, saved_sub.aether);
    EXPECT_EQ(restored_sub.aether_state, saved_sub.aether_state);
    // The composite includes chunk lifecycle/collision availability, which
    // adoption normalizes before boot rebuilds colliders. Only water authority
    // and the unchanged runtime systems above must round-trip in this fixture.
    // Compare cursor changes within the restored world, holding all else fixed.
    restored_world->SetWaterSimWindowCursor(0);
    std::string restored_zero_hash;
    Persistence::WorldStreamingStateSubHashes restored_zero_sub;
    restored.ComputeWorldHashAndSubHashes(restored_zero_hash, restored_zero_sub);
    EXPECT_NE(restored_hash, restored_zero_hash);
    EXPECT_NE(restored_sub.water, restored_zero_sub.water);
    EXPECT_EQ(WaterHashDifferenceSection(restored_sub.water, restored_zero_sub.water),
              "water/cursor");
    EXPECT_EQ(restored_sub.terrain, restored_zero_sub.terrain);
    EXPECT_EQ(restored_sub.entities, restored_zero_sub.entities);
    restored_world->SetWaterSimWindowCursor(66);
    EXPECT_EQ(restored.ComputeWorldHash(), restored_hash);
    EXPECT_EQ(restored.ComputeWorldSubHashes().water, restored_sub.water);
    restored.Shutdown();
}

TEST_F(WorldOpenRefusal, RenderCalendarRefusesEveryInstalledLedgerWithoutClockMetadata) {
    const world::WorldClock zero;
    world::ActiveRegionLedger anchored;
    anchored.set_local_anchor(Vec3(8, 18, 8));
    world::ActiveRegionLedger populated;
    populated.mark_edited({10, -3}, zero);
    populated.set_populated({10, -3}, true);
    world::ActiveRegionLedger frozen({0, 1, 1});
    frozen.mark_edited({10, -3}, zero);
    frozen.schedule(world::WorldClock(1));
    const std::array work{world::RegionWork{{10, -3}, 1, 0, 0, 0}};
    for (const std::uint64_t tick : {2u, 3u}) {
        frozen.schedule(world::WorldClock(tick), {}, work, [](world::RegionKey) { return 17u; });
    }
    const auto version_two = frozen.encode();
    ASSERT_EQ(version_two.at(4), 2);
    // The supported legacy frozen-history envelope has no extension block.
    // Re-seal it using the same stable FNV checksum as the ledger codec.
    auto legacy_frozen = version_two.substr(0, version_two.size() - 8 - 11);
    legacy_frozen[4] = 1;
    const auto checksum = std::stoull(Persistence::StableChecksum(legacy_frozen), nullptr, 16);
    for (unsigned byte = 0; byte < 8; ++byte)
        legacy_frozen.push_back(static_cast<char>((checksum >> (byte * 8)) & 255u));
    const std::vector<std::pair<std::string, std::string>> forms{
        {"empty-v1", world::ActiveRegionLedger{}.encode()},
        {"anchor-only-v1", anchored.encode()},
        {"populated-v1", populated.encode()},
        {"frozen-v1", legacy_frozen},
        {"frozen-v2", version_two}};

    GameSession session;
    session.SetRootPath(RootString());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateTransientWorld("Unchanged", "1337", "default"));
    auto* original_world = session.GetWorldSystem();
    const auto original_ledger = session.GetActiveRegionLedger().encode();
    auto metadata = nlohmann::json::parse(Read(save / "world_info.json"));
    ASSERT_FALSE(metadata.contains("simulationTick"));
    ASSERT_FALSE(metadata.contains("calendar"));
    for (const auto& [name, bytes] : forms) {
        SCOPED_TRACE(name);
        Write(save / "world_info.json", metadata.dump());
        Write(WorldSaveService::active_regions_path(save), bytes);
        world::ActiveRegionLedger decoded;
        std::vector<std::string> errors;
        ASSERT_TRUE(WorldSaveService::load_active_regions(decoded, save, &errors));
        EXPECT_EQ(decoded.encode(), bytes);
        world::WorldClock clock;
        bool required = false;
        std::optional<world::WorldCalendar> rendering;
        ASSERT_TRUE(WorldSaveService::read_clock_metadata(
            save, clock, required, &errors, nullptr, &rendering));
        EXPECT_TRUE(required);
        EXPECT_FALSE(rendering.has_value());
        auto conflicting = metadata;
        conflicting["renderCalendar"] = TestSupport::DefaultCalendarMetadata();
        Write(save / "world_info.json", conflicting.dump());
        const auto before = DiskBytes(root);
        EXPECT_FALSE(WorldSaveService::read_clock_metadata(
            save, clock, required, &errors, nullptr, &rendering));
        ASSERT_FALSE(errors.empty());
        EXPECT_EQ(errors.back(),
                  "Corrupt world metadata: renderCalendar conflicts with active regions.");
        EXPECT_FALSE(rendering.has_value());
        EXPECT_FALSE(WorldSaveService::validate_save(save));
        WorldStreamingState rejected;
        EXPECT_FALSE(WorldSaveService{}.load_world(rejected, save, errors));
        EXPECT_TRUE(rejected.empty());
        EXPECT_FALSE(Persistence::InspectSavedWorld(root, "fixture").error.empty());
        EXPECT_FALSE(session.LoadWorld("fixture"));
        EXPECT_FALSE(session.LoadWorldStateFrom(save));
        EXPECT_EQ(session.GetWorldSystem(), original_world);
        EXPECT_EQ(session.GetActiveRegionLedger().encode(), original_ledger);
        EXPECT_EQ(DiskBytes(root), before);
    }
}

TEST_F(WorldOpenRefusal, RenderCalendarLedgerRefusalUsesPresenceRatherThanPayloadOrStaging) {
    auto metadata = nlohmann::json::parse(Read(save / "world_info.json"));
    metadata["renderCalendar"] = TestSupport::DefaultCalendarMetadata();
    Write(save / "world_info.json", metadata.dump());
    const auto path = WorldSaveService::active_regions_path(save);
    // Staged copies are outside the strict region scan and are never loaded,
    // even if their bytes contain a valid ledger.
    Write(save / "active-regions.arl.tmp.abandoned", world::ActiveRegionLedger{}.encode());
    world::WorldClock clock;
    bool required = true;
    std::optional<world::WorldCalendar> rendering;
    ASSERT_TRUE(
        WorldSaveService::read_clock_metadata(save, clock, required, nullptr, nullptr, &rendering));
    EXPECT_FALSE(required);
    EXPECT_TRUE(rendering.has_value());
    EXPECT_TRUE(WorldSaveService::validate_save(save));
    world::ActiveRegionLedger absent;
    absent.mark_edited({10, -3}, clock);
    ASSERT_TRUE(WorldSaveService::load_active_regions(absent, save));
    EXPECT_TRUE(absent.records().empty());
    for (const std::string& bytes : {std::string{}, std::string{"ARL1"}, std::string{"unknown"}}) {
        SCOPED_TRACE(bytes);
        Write(path, bytes);
        const auto before = DiskBytes(root);
        std::vector<std::string> errors;
        EXPECT_FALSE(WorldSaveService::read_clock_metadata(
            save, clock, required, &errors, nullptr, &rendering));
        ASSERT_FALSE(errors.empty());
        EXPECT_EQ(errors.back(),
                  "Corrupt world metadata: renderCalendar conflicts with active regions.");
        EXPECT_TRUE(required);
        EXPECT_FALSE(rendering.has_value());
        EXPECT_FALSE(WorldSaveService::validate_save(save));
        EXPECT_EQ(DiskBytes(root), before);
    }
    ASSERT_TRUE(fs::remove(path));
    ASSERT_TRUE(fs::create_directory(path));
    std::vector<std::string> errors;
    const auto before = DiskBytes(root);
    EXPECT_FALSE(WorldSaveService::read_clock_metadata(save, clock, required, &errors));
    EXPECT_FALSE(WorldSaveService::validate_save(save));
    EXPECT_EQ(DiskBytes(root), before);
}

TEST_F(WorldOpenRefusal, CatalogSeparatesEmptyUnavailableAndRealMetadataWithoutWriting) {
    const auto before = DiskBytes(root);
    const auto catalog = Persistence::EnumerateSavedWorlds(root);
    ASSERT_TRUE(catalog.error.empty());
    ASSERT_EQ(catalog.worlds.size(), 1u);
    EXPECT_TRUE(catalog.worlds.front().error.empty());
    EXPECT_EQ(catalog.worlds.front().metadata.worldId, "fixture");
    EXPECT_EQ(catalog.worlds.front().metadata.name, "Fixture");
    EXPECT_EQ(catalog.worlds.front().metadata.seed, "1337");
    EXPECT_EQ(catalog.worlds.front().metadata.creationTime, 1);
    const auto empty = Persistence::EnumerateSavedWorlds(root / "absent");
    EXPECT_TRUE(empty.error.empty());
    EXPECT_TRUE(empty.worlds.empty());
    EXPECT_EQ(DiskBytes(root), before);

    fs::remove_all(root / "worlds/saves");
    Write(root / "worlds/saves", "not a directory");
    const auto unavailable = Persistence::EnumerateSavedWorlds(root);
    EXPECT_FALSE(unavailable.error.empty());
    EXPECT_TRUE(unavailable.worlds.empty());
}

TEST_F(WorldOpenRefusal, CancelledValidationNeverReportsAValidSaveOrLeavesPartialAuthority) {
    const auto before = DiskBytes(root);
    std::stop_source cancellation;
    ASSERT_TRUE(cancellation.request_stop());
    ASSERT_TRUE(cancellation.stop_requested());
    const auto inspected =
        Persistence::InspectSavedWorld(root, "fixture", cancellation.get_token());
    EXPECT_FALSE(inspected.error.empty());
    std::vector<std::string> errors;
    EXPECT_FALSE(WorldSaveService::validate_save(save, &errors, cancellation.get_token()));
    ASSERT_FALSE(errors.empty());
    EXPECT_NE(errors.front().find("cancelled"), std::string::npos);
    WorldStreamingState rejected;
    rejected.get_or_create_chunk(IVec3(2, 0, 2));
    errors.clear();
    EXPECT_FALSE(WorldSaveService{}.load_world(rejected, save, errors, cancellation.get_token()));
    EXPECT_TRUE(rejected.empty());
    EXPECT_FALSE(errors.empty());
    EXPECT_EQ(DiskBytes(root), before);
}

TEST_F(WorldOpenRefusal, InvalidMetadataAndMissingWorldRefuseWithoutExceptionsOrWrites) {
    GameSession session;
    session.SetRootPath(RootString());
    session.SetJobSystem(&jobs);
    const auto metadata = nlohmann::json::parse(Read(save / "world_info.json"));
    const std::vector<std::pair<std::string, nlohmann::json>> invalid = {
        {"name", 42},
        {"seed", nullptr},
        {"worldType", "../default"},
        {"creationTime", "yesterday"},
        {"spawnPoint", {{"x", "bad"}, {"y", 1}, {"z", 1}}},
        {"waterSimCursor", -1}};
    for (const auto& [field, value] : invalid) {
        auto damaged = metadata;
        damaged[field] = value;
        Write(save / "world_info.json", damaged.dump());
        const auto before = DiskBytes(root);
        EXPECT_FALSE(session.LoadWorld("fixture")) << field;
        EXPECT_FALSE(session.GetWorldOpenError().empty()) << field;
        EXPECT_FALSE(session.SaveWorld());
        EXPECT_FALSE(session.SaveWorldState());
        EXPECT_EQ(DiskBytes(root), before);
    }
    for (const std::string id : {"missing", "../fixture", "..\\fixture", "/fixture"}) {
        const auto before = DiskBytes(root);
        EXPECT_FALSE(session.LoadWorld(id));
        EXPECT_EQ(DiskBytes(root), before);
    }
}

TEST_F(WorldOpenRefusal, EmbeddedPresetLoadsWithoutTheOriginalNamedPreset) {
    fs::rename(root / "worlds/atlas/presets/default.json", save / "preset.json");
    ASSERT_TRUE(fs::remove(root / "worlds/atlas/presets-rev6/default.json"));
    const auto before = DiskBytes(root);
    GameSession session;
    session.SetRootPath(RootString());
    session.SetJobSystem(&jobs);
    ASSERT_TRUE(session.LoadWorld("fixture")) << session.GetWorldOpenError();
    const auto preset = world::LoadTerrainPreset(save / "preset.json");
    ASSERT_TRUE(preset.ok);
    EXPECT_FALSE(session.PresetContentHash().empty());
    EXPECT_EQ(session.PresetContentHash(), preset.preset_content_hash);
    EXPECT_EQ(session.GetLastLoadedChunkCount(), 1u);
    EXPECT_EQ(DiskBytes(root), before);
}

TEST_F(WorldOpenRefusal, NonEmbeddedAdditionalNamedPresetUsesOriginalFile) {
    const auto preset_path = root / "worlds/atlas/presets/my_hills.json";
    auto preset = nlohmann::json::parse(Read(root / "worlds/atlas/presets-rev6/default.json"));
    preset["generation_params"]["terrain"]["height_offset"] = 17.0f;
    Write(preset_path, preset.dump());
    auto metadata = nlohmann::json::parse(Read(save / "world_info.json"));
    metadata["worldType"] = "my_hills";
    ASSERT_FALSE(metadata.contains("presetIdentity"));
    ASSERT_FALSE(fs::exists(save / "preset.json"));
    ASSERT_FALSE(fs::exists(root / "worlds/atlas/presets-rev6/my_hills.json"));
    const auto identity = Persistence::StableChecksum(preset.dump());

    for (const auto& identity_metadata : {nlohmann::json::object(),
                                          nlohmann::json{{"presetIdentity", identity}},
                                          nlohmann::json{{"presetIdentity", "wrong"}},
                                          nlohmann::json{{"presetIdentity", 123}}}) {
        SCOPED_TRACE(identity_metadata.dump());
        auto saved_metadata = metadata;
        saved_metadata.update(identity_metadata);
        Write(save / "world_info.json", saved_metadata.dump());
        const auto before = DiskBytes(root);

        const auto inspected = Persistence::InspectSavedWorld(root, "fixture");
        EXPECT_EQ(inspected.preset_path, preset_path);
        GameSession session;
        session.SetRootPath(RootString());
        session.SetJobSystem(&jobs);
        if (saved_metadata.contains("presetIdentity") &&
            saved_metadata.at("presetIdentity") != identity) {
            EXPECT_NE(inspected.error.find("identity"), std::string::npos);
            EXPECT_FALSE(session.LoadWorld("fixture"));
            EXPECT_NE(session.GetWorldOpenError().find("identity"), std::string::npos);
            EXPECT_EQ(session.GetWorldSystem(), nullptr);
            EXPECT_EQ(DiskBytes(root), before);
            continue;
        }
        ASSERT_TRUE(inspected.error.empty()) << inspected.error;
        ASSERT_TRUE(session.LoadWorld("fixture")) << session.GetWorldOpenError();
        EXPECT_FLOAT_EQ(session.GetWorldSystem()->get_params().height_offset, 17.0f);
        EXPECT_FALSE(session.GetWorldSystem()->get_params().island_mask_enabled);
        EXPECT_EQ(session.GetLastLoadedChunkCount(), 1u);
        EXPECT_EQ(DiskBytes(root), before);
    }
}

TEST_F(WorldOpenRefusal, EveryFrozenPresetResolvesHistoricalBytesAndChecksIdentity) {
    const fs::path source = LUMINUMBRA_SOURCE_ROOT;
    const auto original_metadata = nlohmann::json::parse(Read(save / "world_info.json"));
    ASSERT_FALSE(fs::exists(save / "preset.json"));
    for (const char* name : {"amplified",
                             "archipelago",
                             "cave_dense",
                             "caverns",
                             "default",
                             "flat_lands",
                             "mountains",
                             "temperate_forest"}) {
        SCOPED_TRACE(name);
        const auto filename = std::string(name) + ".json";
        const auto historical = root / "worlds/atlas/presets-rev6" / filename;
        fs::copy_file(source / "worlds/atlas/presets" / filename,
                      root / "worlds/atlas/presets" / filename,
                      fs::copy_options::overwrite_existing);
        const auto identity =
            Persistence::StableChecksum(nlohmann::json::parse(Read(historical)).dump());
        if (std::string(name) == "cave_dense") {
            EXPECT_EQ(identity, "e70158a4777ee36b");
        }
        auto metadata = original_metadata;
        metadata["worldType"] = name;
        for (const auto& identity_metadata : {nlohmann::json::object(),
                                              nlohmann::json{{"presetIdentity", identity}},
                                              nlohmann::json{{"presetIdentity", "wrong"}}}) {
            SCOPED_TRACE(identity_metadata.dump());
            auto saved_metadata = metadata;
            saved_metadata.update(identity_metadata);
            Write(save / "world_info.json", saved_metadata.dump());
            const auto before = DiskBytes(root);
            const auto inspected = Persistence::InspectSavedWorld(root, "fixture");
            EXPECT_EQ(inspected.preset_path, historical);
            if (saved_metadata.value("presetIdentity", identity) == identity) {
                EXPECT_TRUE(inspected.error.empty()) << inspected.error;
            } else {
                EXPECT_NE(inspected.error.find("identity"), std::string::npos);
            }
            EXPECT_EQ(DiskBytes(root), before);
        }
        // Even metadata without an identity must refuse if its frozen file is absent.
        Write(save / "world_info.json", metadata.dump());
        ASSERT_TRUE(fs::remove(historical));
        const auto before = DiskBytes(root);
        const auto inspected = Persistence::InspectSavedWorld(root, "fixture");
        ExpectMissingPresetReason(inspected.error, historical);
        EXPECT_EQ(DiskBytes(root), before);
    }
}

TEST_F(WorldOpenRefusal, NonEmbeddedRevisionSixHillsPreservesTerrainIdentity) {
    const fs::path source = LUMINUMBRA_SOURCE_ROOT;
    Write(save / "world_info.json",
          Read(source / "test/fixtures/saves/revision6-hills/world_info.json"));
    // SetUp provisions the runtime data once through CopyWorldgenTestData.
    ASSERT_TRUE(fs::is_regular_file(root / "data/common/biomes.json"));
    ASSERT_TRUE(fs::is_directory(root / "data/common/structures"));
    ASSERT_FALSE(fs::exists(save / "preset.json"));
    const auto before = DiskBytes(root);
    const auto historical =
        world::LoadTerrainPreset(root / "worlds/atlas/presets-rev6/default.json");
    const auto shipping = world::LoadTerrainPreset(root / "worlds/atlas/presets/default.json");
    ASSERT_TRUE(historical.ok);
    ASSERT_TRUE(shipping.ok);
    EXPECT_FALSE(historical.params.island_mask_enabled);
    EXPECT_TRUE(shipping.params.island_mask_enabled);
    EXPECT_EQ(
        Persistence::StableChecksum(
            nlohmann::json::parse(Read(root / "worlds/atlas/presets-rev6/default.json")).dump()),
        "026f24207a8ffc7e");

    GameSession session;
    session.SetRootPath(RootString());
    session.SetJobSystem(&jobs);
    ASSERT_TRUE(session.LoadWorld("fixture")) << session.GetWorldOpenError();
    const auto& params = session.GetWorldSystem()->get_params();
    EXPECT_FALSE(params.island_mask_enabled);
    EXPECT_FLOAT_EQ(params.base_frequency, 0.01f);
    EXPECT_FLOAT_EQ(params.base_amplitude, 34.0f);
    EXPECT_FLOAT_EQ(params.height_offset, 5.0f);
    EXPECT_TRUE(params.hydro_enabled);
    EXPECT_TRUE(params.biomes_enabled);
    EXPECT_TRUE(params.biome_relief_enabled);
    EXPECT_TRUE(params.rivers_enabled);
    EXPECT_TRUE(params.lakes_enabled);
    EXPECT_TRUE(params.cliffs_enabled);
    EXPECT_TRUE(params.structures_enabled);
    EXPECT_TRUE(params.surface_breaks_enabled);
    EXPECT_EQ(World::ComputeTerrainParamsHash(params, session.GetWorldSystem()->get_seed()),
              16636010052247870746ull);
    EXPECT_EQ(session.GetLastLoadedChunkCount(), 1u);
    EXPECT_EQ(DiskBytes(root), before);
}

TEST_F(WorldOpenRefusal, NonEmbeddedSaveWithoutIdentityNeverUsesShippingPreset) {
    // The fixture's old metadata has no identity. Removing shipping content
    // must not affect inspection or opening of an existing world.
    fs::remove(root / "worlds/atlas/presets/default.json");
    const auto before = DiskBytes(root);
    const auto inspected = Persistence::InspectSavedWorld(root, "fixture");
    ASSERT_TRUE(inspected.error.empty()) << inspected.error;
    EXPECT_EQ(inspected.preset_path, root / "worlds/atlas/presets-rev6/default.json");
    GameSession session;
    session.SetRootPath(RootString());
    session.SetJobSystem(&jobs);
    ASSERT_TRUE(session.LoadWorld("fixture")) << session.GetWorldOpenError();
    EXPECT_FALSE(session.GetWorldSystem()->get_params().island_mask_enabled);
    EXPECT_EQ(DiskBytes(root), before);
}

TEST_F(WorldOpenRefusal, MissingHistoricalPresetRefusesRatherThanUsingShipping) {
    const auto historical = root / "worlds/atlas/presets-rev6/default.json";
    ASSERT_TRUE(fs::remove(historical));
    const auto before = DiskBytes(root);
    const auto inspected = Persistence::InspectSavedWorld(root, "fixture");
    ExpectMissingPresetReason(inspected.error, historical);
    GameSession session;
    session.SetRootPath(RootString());
    session.SetJobSystem(&jobs);
    EXPECT_FALSE(session.LoadWorld("fixture"));
    ExpectMissingPresetReason(session.GetWorldOpenError(), historical);
    EXPECT_EQ(session.GetWorldSystem(), nullptr);
    EXPECT_EQ(DiskBytes(root), before);
}

TEST_F(WorldOpenRefusal, RecordedPresetIdentityMismatchRefusesBeforeGeneration) {
    for (const bool embedded : {false, true}) {
        if (embedded)
            fs::copy_file(root / "worlds/atlas/presets/default.json", save / "preset.json");
        for (const auto& identity : {nlohmann::json("wrong"), nlohmann::json(123)}) {
            auto metadata = nlohmann::json::parse(Read(save / "world_info.json"));
            metadata["presetIdentity"] = identity;
            Write(save / "world_info.json", metadata.dump());
            const auto before = DiskBytes(root);
            const auto inspected = Persistence::InspectSavedWorld(root, "fixture");
            EXPECT_NE(inspected.error.find("identity"), std::string::npos);
            GameSession session;
            session.SetRootPath(RootString());
            session.SetJobSystem(&jobs);
            EXPECT_FALSE(session.LoadWorld("fixture"));
            EXPECT_EQ(session.GetWorldSystem(), nullptr);
            EXPECT_EQ(DiskBytes(root), before);
        }
    }
}

TEST_F(WorldOpenRefusal, CreateEditSaveRestartAndSwitchPreserveAuthority) {
    std::string id;
    std::vector<float> edited_sdf;
    {
        GameSession created;
        created.SetRootPath(RootString());
        created.SetJobSystem(&jobs);
        ASSERT_TRUE(created.CreateWorld("Edited <world>", "424242", "default"));
        id = created.GetMetadata().worldId;
        auto* world = created.GetWorldSystem();
        world->dispatch_generation_jobs(std::vector<IVec3>{{0, 0, 0}});
        world->wait_for_streaming_jobs();
        auto chunks = world->snapshot_streamed_chunks();
        ASSERT_EQ(chunks.size(), 1u);
        const auto original_sdf = chunks.front()->sdf_data;
        ASSERT_GT(world->EditTerrainVoxel(Vec3(8, 8, 8), 3.0f, false, created.GetPhysicsSystem()),
                  0);
        world->wait_for_streaming_jobs();
        edited_sdf = chunks.front()->sdf_data;
        ASSERT_NE(edited_sdf, original_sdf);
        created.SetSpawnPoint(Vec3(8, 18, 8));
        world::WorldStateSaveReport report;
        ASSERT_TRUE(created.SaveWorldState(&report));
        ASSERT_TRUE(report.saved);
        ASSERT_TRUE(created.SaveWorld());
    }
    const auto before = DiskBytes(root);
    GameSession reopened;
    reopened.SetRootPath(RootString());
    reopened.SetJobSystem(&jobs);
    for (int switch_count = 0; switch_count < 6; ++switch_count) {
        ASSERT_TRUE(reopened.LoadWorld(id)) << reopened.GetWorldOpenError();
        EXPECT_EQ(reopened.GetMetadata().spawnPoint, Vec3(8, 18, 8));
        auto* world = reopened.GetWorldSystem();
        // A fully restored batch is successful with no jobs. It must preserve the edit.
        ASSERT_FALSE(world->dispatch_generation_jobs(std::vector<IVec3>{{0, 0, 0}}).counter);
        auto chunks = world->snapshot_streamed_chunks();
        const auto edited = std::find_if(chunks.begin(), chunks.end(), [](const auto& chunk) {
            return chunk->get_coords() == IVec3(0, 0, 0);
        });
        ASSERT_NE(edited, chunks.end());
        EXPECT_EQ((*edited)->sdf_data, edited_sdf);
        EXPECT_EQ((*edited)->sdf_provenance(), ChunkSdfProvenance::LoadedOrEdited);
        // Replace the world while background generation is active; its dependencies must live
        // until the workers complete, and the next world must not inherit these chunks.
        world->dispatch_generation_jobs(
            std::vector<IVec3>{{100, 0, 100}, {101, 0, 100}, {102, 0, 100}});
        ASSERT_TRUE(reopened.LoadWorld("fixture")) << reopened.GetWorldOpenError();
        EXPECT_EQ(reopened.GetLastLoadedChunkCount(), 1u);
        EXPECT_EQ(reopened.GetWorldSystem()->snapshot_streamed_chunks().front()->sdf_data.front(),
                  1.0f);
    }
    EXPECT_EQ(DiskBytes(root), before);
}

TEST_F(WorldOpenRefusal, IndexedOpenValidatesOnceWithoutCompressionAcrossRepeatedOpens) {
    for (const std::uint32_t count : {4u, 32u}) {
        SCOPED_TRACE(count);
        Persistence::SyntheticSaveSpec spec;
        spec.world_id = "indexed-open-" + std::to_string(count);
        spec.chunk_count = count;
        spec.payload = Persistence::FixturePayload::Flat;
        spec.preset_source = fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets/default.json";
        const auto report = Persistence::WriteSyntheticSave(spec, root);
        ASSERT_TRUE(report.ok) << report.error;
        ASSERT_TRUE(Luminumbra::Test::PublishIndexedSave(report.save_dir, [] { return true; }));
        const auto before = DiskBytes(root);
        GameSession reopened;
        reopened.SetRootPath(RootString());
        reopened.SetJobSystem(&jobs);
        for (int opening = 0; opening < 3; ++opening) {
            SCOPED_TRACE(opening);
            const auto validations = Persistence::detail::decode_count_for_testing();
            const auto decodes = WorldSaveService::chunk_record_decode_count_for_testing();
            const auto compressions = WorldSaveService::record_compression_count_for_testing();
            const auto parses = Persistence::saved_json_parse_count_for_testing();
            ASSERT_TRUE(reopened.LoadWorld(spec.world_id)) << reopened.GetWorldOpenError();
            EXPECT_EQ(Persistence::detail::decode_count_for_testing(), validations + 1);
            EXPECT_EQ(WorldSaveService::chunk_record_decode_count_for_testing(), decodes + count);
            EXPECT_EQ(WorldSaveService::record_compression_count_for_testing(), compressions);
            // Metadata, embedded preset and manifest: each DOM is parsed once per open.
            EXPECT_EQ(Persistence::saved_json_parse_count_for_testing(), parses + 3);
            EXPECT_EQ(DiskBytes(root), before);
            WorldSaveService::ChunkReader reader;
            ASSERT_NO_FATAL_FAILURE(ExpectWarmReaderPasses(report.save_dir, count, reader));
            ASSERT_NO_FATAL_FAILURE(
                ExpectReaderRefusesChangedBytes(root, report.save_dir, count, reader));
            EXPECT_EQ(DiskBytes(root), before);
        }
        const auto region = WorldSaveService::region_file_path(report.save_dir, 0, 0);
        auto damaged = Read(region);
        ASSERT_FALSE(damaged.empty());
        damaged.back() ^= 1;
        Write(region, damaged);
        const auto damaged_bytes = DiskBytes(root);
        const auto* previous_world = reopened.GetWorldSystem();
        EXPECT_FALSE(reopened.LoadWorld(spec.world_id));
        EXPECT_FALSE(reopened.GetWorldOpenError().empty());
        EXPECT_EQ(reopened.GetWorldSystem(), previous_world);
        EXPECT_EQ(DiskBytes(root), damaged_bytes);
        EXPECT_FALSE(Persistence::InspectSavedWorld(root, spec.world_id).error.empty());
    }
}

TEST_F(WorldOpenRefusal, ParsedSavedJsonReuseStillChecksChangedBytesAndLimits) {
    const Persistence::SavedJsonReadScope scope;
    nlohmann::json document;
    bool missing = false;
    Persistence::SavedJsonFailure failure = Persistence::SavedJsonFailure::None;
    const auto path = save / "world_info.json";
    const auto parses = Persistence::saved_json_parse_count_for_testing();
    ASSERT_TRUE(Persistence::ReadSavedJson(path, 4096, document, missing));
    ASSERT_FALSE(missing);
    const auto original = document;
    ASSERT_TRUE(Persistence::ReadSavedJson(path, 4096, document, missing));
    EXPECT_EQ(document, original);
    EXPECT_EQ(Persistence::saved_json_parse_count_for_testing(), parses + 1);
    EXPECT_FALSE(Persistence::ReadSavedJson(path, 1, document, missing, true, &failure));
    EXPECT_EQ(failure, Persistence::SavedJsonFailure::Limit);
    Write(path, "{");
    EXPECT_FALSE(Persistence::ReadSavedJson(path, 4096, document, missing, true, &failure));
    EXPECT_EQ(failure, Persistence::SavedJsonFailure::Malformed);
    EXPECT_EQ(Persistence::saved_json_parse_count_for_testing(), parses + 2);
}

TEST_F(WorldOpenRefusal, SavedWorldLifecycleFixtureSwitchesWithoutWritingAnything) {
    Persistence::SyntheticSaveSpec spec;
    spec.world_id = "synthetic-lifecycle";
    spec.chunk_count = FixtureChunksFromEnv("LUMIN_LIFECYCLE_FIXTURE_CHUNKS", 64);
    spec.preset_source = fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets/default.json";
    const auto report = Persistence::WriteSyntheticSave(spec, root);
    ASSERT_TRUE(report.ok) << report.error;
    const auto before = DiskBytes(root);
    GameSession reopened;
    reopened.SetRootPath(RootString());
    reopened.SetJobSystem(&jobs);
    for (int switch_count = 0; switch_count < 6; ++switch_count) {
        ASSERT_TRUE(reopened.LoadWorld("synthetic-lifecycle")) << reopened.GetWorldOpenError();
        ASSERT_TRUE(reopened.LoadWorld("fixture")) << reopened.GetWorldOpenError();
    }
    EXPECT_EQ(DiskBytes(root), before);
}

TEST_F(WorldOpenRefusal, InterruptedMetadataSaveKeepsPreviousBytesAndCanRetry) {
    GameSession session;
    session.SetRootPath(RootString());
    session.SetJobSystem(&jobs);
    ASSERT_TRUE(session.LoadWorld("fixture"));
    const auto before = DiskBytes(root);
    session.SetSpawnPoint(Vec3(10, 20, 30));
    WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    EXPECT_FALSE(session.SaveWorld());
    WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
    EXPECT_EQ(DiskBytes(root), before);
    ASSERT_TRUE(session.SaveWorld());
    const auto saved = Persistence::InspectSavedWorld(root, "fixture");
    EXPECT_TRUE(saved.error.empty());
    EXPECT_EQ(saved.metadata.spawnPoint, Vec3(10, 20, 30));
}

TEST_F(WorldOpenRefusal, MenuWorldCreatesNoSaveAndClearsPreviousWorldState) {
    const auto before = DiskBytes(root);
    GameSession session;
    session.SetRootPath(RootString());
    session.SetJobSystem(&jobs);
    ASSERT_TRUE(session.LoadWorld("fixture"));
    ASSERT_TRUE(session.CreateTransientWorld("Menu Vista", "424242", "default"));
    EXPECT_TRUE(session.LoadWorldState());
    EXPECT_TRUE(session.GetWorldSaveDir().empty());
    EXPECT_TRUE(session.GetWorldSystem()->snapshot_streamed_chunks().empty());
    EXPECT_FALSE(session.SaveWorld());
    EXPECT_FALSE(session.SaveWorldState());
    EXPECT_EQ(DiskBytes(root), before);
}

TEST_F(WorldOpenRefusal, AbsentLedgerPreservesSchedulerAndRestoresClockWithoutRegionState) {
    const world::RegionSchedulerConfig scheduler{10, 3, 2};
    world::ActiveRegionLedger ledger(scheduler);
    ledger.set_local_anchor(Vec3(8, 18, 8));
    ledger.pin({0, 0}, true, world::WorldClock{});
    const world::WorldClock restored(45);
    std::vector<std::string> errors;
    ASSERT_FALSE(fs::exists(WorldSaveService::active_regions_path(save)));
    ASSERT_TRUE(WorldSaveService::load_active_regions(ledger, save, &errors, restored));
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(ledger.config(), scheduler);
    EXPECT_EQ(ledger.tick(), restored.tick());
    EXPECT_EQ(ledger.header_tick(), restored.tick());
    EXPECT_TRUE(ledger.records().empty());
    EXPECT_FALSE(ledger.local_anchor().has_value());
}

TEST_F(WorldOpenRefusal, RuntimeHelloMatchesNewWorldCalendarWithActiveRegionsOnAndOff) {
    for (const bool active_regions : {false, true}) {
        SCOPED_TRACE(active_regions);
        const nlohmann::json systems = {
            {"sim", {{"active_regions", {{"enabled", active_regions}}}}}};
        Write(root / "data/common/systems.json", systems.dump());
        for (const bool use_override : {false, true}) {
            SCOPED_TRACE(use_override);
            ServerCliOptions options;
            options.root = RootString();
            options.seed = "1337";
            options.preset = "default";
            if (use_override)
                options.systems_config_override =
                    luminumbra::core::SystemConfig::FromJsonString(systems.dump());
            const auto hello = RuntimeHelloFrom(options);
            GameSession host;
            host.SetRootPath(RootString());
            host.SetJobSystem(&jobs);
            host.ApplySystemConfig(luminumbra::core::SystemConfig::FromJsonString(systems.dump()));
            host.SetFiniteHydrologyEnabled(RunnerConfigFrom(options).finite_hydrology);
            ASSERT_TRUE(host.CreateWorld("Network calendar", options.seed, options.preset));
            ASSERT_TRUE(host.LoadWorldState());
            EXPECT_EQ(host.ActiveRegionsEnabled(), active_regions);
            EXPECT_EQ(host.GetRenderCalendar(), world::WorldCalendar{});
            EXPECT_EQ(hello.system_config_identity, host.SystemConfigIdentity());
            auto expected = hello;
            expected.system_config_identity = host.SystemConfigIdentity();
            expected.preset_content_hash = host.PresetContentHash();
            Net::HelloMsg decoded;
            ASSERT_TRUE(Net::DecodeHello(Net::EncodeHello(hello), decoded));
            EXPECT_TRUE(Net::HelloMatches(expected, decoded));
        }
    }
}

TEST_F(WorldOpenRefusal, FreshRunnerBootAndAbsentLedgerReloadPreserveSchedulerIdentity) {
    const world::RegionSchedulerConfig scheduler{10, 3, 2};
    Server::ServerWorldRunnerConfig config;
    config.root_path = RootString();
    config.systems_config_override = luminumbra::core::SystemConfig::Defaults();
    config.region_scheduler = scheduler;
    config.surface_radius = 0;
    config.collision_radius = 0;
    config.test_streaming_radius_cap = 1;
    config.autosave_interval_ticks = 0;
    GameSession expected;
    expected.ApplySystemConfig(*config.systems_config_override);
    expected.SetRegionSchedulerConfig(scheduler);
    Server::ServerWorldRunner runner(config);
    ASSERT_TRUE(runner.Boot()) << runner.GetBootError();
    ASSERT_NE(runner.Session(), nullptr);
    ASSERT_TRUE(runner.Session()->ActiveRegionsEnabled());
    EXPECT_EQ(runner.Session()->GetActiveRegionLedger().config(), scheduler);
    EXPECT_EQ(runner.Session()->SystemConfigIdentity(), expected.SystemConfigIdentity());
    EXPECT_NE(runner.Session()->SystemConfigIdentity(), GameSession{}.SystemConfigIdentity());
    const auto ledger_path =
        WorldSaveService::active_regions_path(runner.Session()->GetWorldSaveDir());
    ASSERT_TRUE(WorldSaveService::remove_member(runner.Session()->GetWorldSaveDir(),
                                                "chunks/region/active-regions.arl"));
    ASSERT_FALSE(fs::exists(ledger_path));
    ASSERT_TRUE(runner.Session()->LoadWorldState()) << runner.Session()->GetWorldOpenError();
    EXPECT_EQ(runner.Session()->GetActiveRegionLedger().config(), scheduler);
    EXPECT_EQ(runner.Session()->SystemConfigIdentity(), expected.SystemConfigIdentity());
    EXPECT_EQ(runner.TickCount(), 0u);
    config.world_id = runner.Session()->GetMetadata().worldId;
    runner.Shutdown();
    // A subsequent runner with no override must identify the saved effective scheduler.
    config.region_scheduler.reset();
    const auto before = DiskBytes(root);
    Server::ServerWorldRunner loaded(config);
    ASSERT_TRUE(loaded.Boot()) << loaded.GetBootError();
    EXPECT_EQ(loaded.Session()->GetActiveRegionLedger().config(), scheduler);
    EXPECT_EQ(loaded.Session()->SystemConfigIdentity(), expected.SystemConfigIdentity());
    EXPECT_EQ(DiskBytes(root), before);
    loaded.Shutdown();
}

TEST_F(WorldOpenRefusal, PersistedSchedulerIdentityRefusesDifferentAdmissionAndReplayRules) {
    // Keep the calendar identical to fresh sessions so only scheduler rules differ.
    auto metadata = nlohmann::json::parse(Read(save / "world_info.json"));
    world::WorldClock{}.write_metadata(metadata);
    Write(save / "world_info.json", metadata.dump());
    // Each field changes independently while both sessions start with default member storage.
    // Identity must observe the decoded ledger rather than that original storage.
    world::RegionSchedulerConfig work_limit, hold_ticks, reduced_shift;
    --work_limit.work_limit;
    ++hold_ticks.hold_ticks;
    ++reduced_shift.reduced_shift;
    GameSession baseline;
    baseline.SetRootPath(RootString());
    baseline.SetJobSystem(&jobs);
    ASSERT_TRUE(baseline.LoadWorld("fixture")) << baseline.GetWorldOpenError();
    const auto baseline_identity = baseline.SystemConfigIdentity();
    for (const auto scheduler : {work_limit, hold_ticks, reduced_shift}) {
        SCOPED_TRACE(scheduler.work_limit);
        SCOPED_TRACE(scheduler.hold_ticks);
        SCOPED_TRACE(static_cast<unsigned>(scheduler.reduced_shift));
        world::ActiveRegionLedger persisted(scheduler);
        persisted.set_local_anchor(Vec3(8, 18, 8));
        Write(WorldSaveService::active_regions_path(save), persisted.encode());
        const auto before = DiskBytes(root);
        GameSession loaded, expected;
        loaded.SetRootPath(RootString());
        loaded.SetJobSystem(&jobs);
        ASSERT_TRUE(loaded.LoadWorld("fixture")) << loaded.GetWorldOpenError();
        expected.SetRegionSchedulerConfig(scheduler);
        EXPECT_EQ(loaded.GetActiveRegionLedger().config(), scheduler);
        EXPECT_EQ(loaded.SystemConfigIdentity(), expected.SystemConfigIdentity());
        ASSERT_NE(loaded.SystemConfigIdentity(), baseline_identity);
        EXPECT_EQ(DiskBytes(root), before);

        Net::LockstepConfig local;
        local.local_client_id = 0;
        local.peer_client_id = 1;
        local.system_config_identity = baseline_identity;
        local.preset_content_hash = loaded.PresetContentHash();
        Net::HelloMsg hello;
        hello.seed = local.seed;
        hello.preset = local.preset;
        hello.tick_rate_hz = local.tick_rate_hz;
        hello.client_id = 1;
        hello.system_config_identity = loaded.SystemConfigIdentity();
        hello.preset_content_hash = loaded.PresetContentHash();
        auto expected_hello = hello;
        expected_hello.system_config_identity = baseline_identity;
        expected_hello.preset_content_hash = loaded.PresetContentHash();
        EXPECT_FALSE(Net::HelloMatches(expected_hello, hello)); // TCP admission
        auto [host_transport, peer_transport] = Net::MakeLoopbackPair();
        Net::LockstepSession host(local, host_transport.get(), {});
        ASSERT_TRUE(peer_transport->SendFrame(Net::EncodeHello(hello)));
        EXPECT_FALSE(host.Handshake());
        EXPECT_FALSE(host.Status().handshaken);

        Replay::ReplayHeader header;
        header.system_config_identity = baseline_identity;
        header.preset_content_hash = loaded.PresetContentHash();
        const auto replay_path = root / "scheduler.lrec1";
        Replay::ReplayWriter writer;
        ASSERT_TRUE(writer.Open(replay_path.string(), header));
        ASSERT_TRUE(writer.Finalize(0));
        const auto replay = Replay::ReadReplay(replay_path.string());
        ASSERT_TRUE(replay.has_value());
        if (!replay.has_value())
            return;
        EXPECT_TRUE(Replay::ConfigurationMatches(
            replay->header, baseline_identity, header.preset_content_hash));
        EXPECT_FALSE(Replay::ConfigurationMatches(
            replay->header, loaded.SystemConfigIdentity(), header.preset_content_hash));
        auto matching_header = replay->header;
        matching_header.system_config_identity = loaded.SystemConfigIdentity();
        matching_header.preset_content_hash = loaded.PresetContentHash();
        EXPECT_TRUE(Replay::ConfigurationMatches(
            matching_header, expected.SystemConfigIdentity(), header.preset_content_hash));
        EXPECT_EQ(baseline.GetSimulationTickCount(), 0u);
        EXPECT_EQ(loaded.GetSimulationTickCount(), 0u);
        ASSERT_TRUE(fs::remove(replay_path));
    }
}

TEST_F(WorldOpenRefusal, LoadedSchedulerSurvivesTransientCreateAndAbsentLedgerReplacement) {
    // Fresh and reloaded sessions must resolve the same calendar in this scheduler test.
    auto metadata = nlohmann::json::parse(Read(save / "world_info.json"));
    world::WorldClock{}.write_metadata(metadata);
    Write(save / "world_info.json", metadata.dump());
    const world::RegionSchedulerConfig scheduler{10, 3, 2};
    world::ActiveRegionLedger persisted(scheduler);
    persisted.set_local_anchor(Vec3(8, 18, 8));
    Write(WorldSaveService::active_regions_path(save), persisted.encode());
    GameSession session, expected;
    session.SetRootPath(RootString());
    session.SetJobSystem(&jobs);
    expected.SetRegionSchedulerConfig(scheduler);
    ASSERT_TRUE(session.LoadWorld("fixture")) << session.GetWorldOpenError();
    const auto expect_scheduler = [&]() {
        EXPECT_EQ(session.GetActiveRegionLedger().config(), scheduler);
        EXPECT_EQ(session.SystemConfigIdentity(), expected.SystemConfigIdentity());
    };
    expect_scheduler();
    ASSERT_TRUE(session.CreateTransientWorld("Scheduler menu", "1337", "default"));
    expect_scheduler();
    ASSERT_TRUE(session.LoadWorldState());
    expect_scheduler();
    // Exercise direct state load into an existing transient world, followed by an absent file.
    ASSERT_TRUE(session.LoadWorldStateFrom(save)) << session.GetWorldOpenError();
    expect_scheduler();
    ASSERT_TRUE(fs::remove(WorldSaveService::active_regions_path(save)));
    ASSERT_TRUE(session.LoadWorldStateFrom(save)) << session.GetWorldOpenError();
    expect_scheduler();
    ASSERT_TRUE(session.CreateWorld("Scheduler reset", "1337", "default"));
    expect_scheduler();
    ASSERT_TRUE(session.LoadWorldState());
    expect_scheduler();
}

TEST_F(WorldOpenRefusal, ExplicitSchedulerConflictRefusesLoadAndRunnerBootWithoutWriting) {
    auto metadata = nlohmann::json::parse(Read(save / "world_info.json"));
    world::WorldClock{}.write_metadata(metadata);
    Write(save / "world_info.json", metadata.dump());
    const world::RegionSchedulerConfig scheduler{10, 3, 2};
    world::ActiveRegionLedger persisted(scheduler);
    persisted.set_local_anchor(Vec3(8, 18, 8));
    Write(WorldSaveService::active_regions_path(save), persisted.encode());
    const auto before = DiskBytes(root);
    // A matching explicit override is accepted and preserves all decoded scheduler state.
    GameSession matching;
    matching.SetRootPath(RootString());
    matching.SetJobSystem(&jobs);
    matching.SetRegionSchedulerConfig(scheduler);
    const auto matching_identity = matching.SystemConfigIdentity();
    ASSERT_TRUE(matching.LoadWorld("fixture")) << matching.GetWorldOpenError();
    EXPECT_EQ(matching.SystemConfigIdentity(), matching_identity);
    EXPECT_EQ(matching.GetActiveRegionLedger().canonical_bytes(), persisted.canonical_bytes());
    const std::string conflict =
        "Incompatible configuration: persisted region scheduler differs from explicit override.";
    auto work_limit = scheduler, hold_ticks = scheduler, reduced_shift = scheduler;
    ++work_limit.work_limit;
    ++hold_ticks.hold_ticks;
    ++reduced_shift.reduced_shift;
    for (const auto override_config :
         {work_limit, hold_ticks, reduced_shift, world::RegionSchedulerConfig{}}) {
        GameSession refused;
        refused.SetRootPath(RootString());
        refused.SetJobSystem(&jobs);
        refused.SetRegionSchedulerConfig(override_config);
        const auto requested_identity = refused.SystemConfigIdentity();
        EXPECT_FALSE(refused.LoadWorld("fixture"));
        EXPECT_EQ(refused.GetWorldOpenError(), conflict);
        EXPECT_EQ(refused.GetActiveRegionLedger().config(), override_config);
        EXPECT_EQ(refused.SystemConfigIdentity(), requested_identity);
        EXPECT_FALSE(refused.SaveWorld());
        EXPECT_EQ(DiskBytes(root), before);
        // The direct state-load entry point enforces the same conflict policy.
        ASSERT_TRUE(refused.CreateTransientWorld("Scheduler conflict", "1337", "default"));
        EXPECT_FALSE(refused.LoadWorldStateFrom(save));
        EXPECT_EQ(refused.GetWorldOpenError(), conflict);
        EXPECT_EQ(refused.GetActiveRegionLedger().config(), override_config);
        EXPECT_EQ(refused.SystemConfigIdentity(), requested_identity);
        EXPECT_EQ(DiskBytes(root), before);
    }
    Server::ServerWorldRunnerConfig config;
    config.root_path = RootString();
    config.world_id = "fixture";
    config.systems_config_override = luminumbra::core::SystemConfig::Defaults();
    config.region_scheduler = world::RegionSchedulerConfig{};
    Server::ServerWorldRunner runner(config);
    EXPECT_FALSE(runner.Boot());
    EXPECT_EQ(runner.GetBootError(), conflict);
    EXPECT_EQ(runner.Session(), nullptr);
    EXPECT_EQ(DiskBytes(root), before);
}

TEST_F(WorldOpenRefusal, ServerAutosaveAndFullSnapshotCarryTheAbsoluteClock) {
    Write(root / "data/common/systems.json", R"({"sim":{"active_regions":{"enabled":true}}})");
    Server::ServerWorldRunnerConfig config;
    config.root_path = RootString();
    config.world_id = "fixture";
    config.surface_radius = 0;
    config.collision_radius = 0;
    config.autosave_interval_ticks = 5;
    Server::ServerWorldRunner original(config);
    ASSERT_TRUE(original.Boot()) << original.GetBootError();
    ASSERT_TRUE(original.Session()->ActiveRegionsEnabled());
    // Boot warms the production streaming radius independently of surface_radius.
    // Keep this clock/persistence oracle bounded to its saved chunk and nearby cells.
    original.Session()->GetWorldSystem()->clear_world(original.Session()->GetPhysicsSystem());
    original.Session()->GetWorldSystem()->debug_set_streaming_radius_cap(1);
    ASSERT_TRUE(original.Session()->LoadWorldState());
    // A single runner tick must autosave at absolute tick 45, even though
    // this RunFixedTicks call has executed only one tick.
    for (int i = 0; i < 44; ++i)
        ASSERT_EQ(
            original.Session()->TickSimulation(original.Session()->GetSimulationClock().fixed_dt()),
            1u);
    original.RunFixedTicks(1);
    EXPECT_EQ(Persistence::InspectSavedWorld(root, "fixture").clock.tick(), 45u);
    ASSERT_GT(original.SaveFullSnapshot(), 0u);
    const auto before = DiskBytes(root);
    Server::ServerWorldRunner loaded(config);
    ASSERT_TRUE(loaded.Boot()) << loaded.GetBootError();
    EXPECT_EQ(loaded.TickCount(), original.TickCount());
    EXPECT_EQ(DiskBytes(root), before);
    const auto compare = [&]() {
        EXPECT_EQ(original.Session()->GetWindFieldSystem()->ComputeWindSubHash(),
                  loaded.Session()->GetWindFieldSystem()->ComputeWindSubHash());
        EXPECT_EQ(original.Session()->GetWeatherSystem()->ComputeWeatherSubHash(),
                  loaded.Session()->GetWeatherSystem()->ComputeWeatherSubHash());
        EXPECT_EQ(original.Session()->GetAetherFieldSystem()->ComputeAetherSubHash(),
                  loaded.Session()->GetAetherFieldSystem()->ComputeAetherSubHash());
        EXPECT_EQ(original.Session()->GetWorldClock().canonical_bytes(),
                  loaded.Session()->GetWorldClock().canonical_bytes());
    };
    compare();
    loaded.Session()->GetWorldSystem()->clear_world(loaded.Session()->GetPhysicsSystem());
    loaded.Session()->GetWorldSystem()->debug_set_streaming_radius_cap(1);
    ASSERT_TRUE(loaded.Session()->LoadWorldState());
    original.RunFixedTicks(5);
    loaded.RunFixedTicks(5);
    compare();
    bool callback = false;
    original.Session()->GetSimulationEventBus().subscribe([&](const auto&) {
        callback = true;
        EXPECT_EQ(original.SaveFullSnapshot(), 0u);
    });
    original.Session()->GetSimulationEventBus().publish(51, "save", "");
    original.RunFixedTicks(1);
    EXPECT_TRUE(callback);
    original.Shutdown();
    EXPECT_EQ(Persistence::InspectSavedWorld(root, "fixture").clock.tick(), 51u);
    loaded.Shutdown();
}

class SnapshotPresetFiles {
public:
    explicit SnapshotPresetFiles(const fs::path& destination)
        : directory(destination)
        , preset_bytes(Read(directory / "preset.json"))
        , metadata_bytes(Read(directory / "world_info.json"))
        , other_preset(nlohmann::json::parse(preset_bytes))
        , other_metadata(nlohmann::json::parse(metadata_bytes)) {
        other_preset["generation_params"]["terrain"]["height_offset"] = 123.0;
        other_metadata["presetIdentity"] = Persistence::StableChecksum(other_preset.dump());
    }

    void Rewrite(const char* changed) const {
        SCOPED_TRACE(directory.generic_string());
        if (std::string(changed) != "identity") {
            ASSERT_NO_FATAL_FAILURE(Write(directory / "preset.json", other_preset.dump()));
        }
        if (std::string(changed) != "preset") {
            ASSERT_NO_FATAL_FAILURE(Write(directory / "world_info.json", other_metadata.dump()));
        }
    }

    void Restore() const {
        SCOPED_TRACE(directory.generic_string());
        ASSERT_NO_FATAL_FAILURE(Write(directory / "preset.json", preset_bytes));
        ASSERT_NO_FATAL_FAILURE(Write(directory / "world_info.json", metadata_bytes));
    }

private:
    fs::path directory;
    std::string preset_bytes;
    std::string metadata_bytes;
    nlohmann::json other_preset;
    nlohmann::json other_metadata;
};

class SnapshotDestinationBytes {
public:
    explicit SnapshotDestinationBytes(const fs::path& destination)
        : directory(destination)
        , bytes(DiskBytes(directory)) {}

    void ExpectUnchanged() const {
        SCOPED_TRACE(directory.generic_string());
        EXPECT_EQ(DiskBytes(directory), bytes);
    }

private:
    fs::path directory;
    std::map<std::string, std::string> bytes;
};

void PreparePendingSnapshot(GameSession& session,
                            std::shared_ptr<Chunk>& chunk,
                            bool existing_snapshot) {
    SCOPED_TRACE("prepare pending snapshot");
    chunk = std::make_shared<Chunk>(IVec3(0, 0, 0));
    chunk->set_state(ChunkState::Idle);
    chunk->heightmap_data = {8.0f, 9.0f, 10.0f};
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
    auto& registry = session.GetRegistry();
    const auto plant = registry.create();
    registry.emplace<Components::PlantTag>(plant);
    registry.emplace<Components::PlantGenomeComponent>(plant).genes.fill(0.5f);
    registry.emplace<Components::PlantGrowthComponent>(plant);
    registry.emplace<Components::TransformComponent>(plant).position =
        session.GetMetadata().spawnPoint;
    auto* energy = session.GetEnergyFieldState();
    ASSERT_NE(energy, nullptr);
    energy->SetAnchorCell(0, 0);
    energy->QueueDeposit(1, 0, 0, 0, 60000);
    ASSERT_NO_FATAL_FAILURE(ApplySnapshotTestEnergy(session, 0));
    if (existing_snapshot) {
        ASSERT_TRUE(session.SaveWorldState());
        ASSERT_FALSE(chunk->is_voxel_data_dirty());
    }
    ASSERT_EQ(WorldSaveService::has_chunk_snapshot(session.GetWorldSaveDir()), existing_snapshot);

    chunk->heightmap_data.front() = 42.0f;
    chunk->mark_voxel_data_dirty();
    registry.get<Components::PlantGrowthComponent>(plant).growth_points = 123;
    energy->QueueDeposit(2, 0, 0, 0, 1000);
    ASSERT_NO_FATAL_FAILURE(ApplySnapshotTestEnergy(session, 0));
}

class PendingSnapshotState {
public:
    explicit PendingSnapshotState(const GameSession& session)
        : plant_hash(session.ComputePlantSubHash())
        , energy_bytes(session.GetEnergyFieldState()->CanonicalBytes())
        , ledger_bytes(session.GetActiveRegionLedger().canonical_bytes())
        , clock_bytes(session.GetWorldClock().canonical_bytes()) {}

    void ExpectUnchanged(const GameSession& session, const Chunk& chunk) const {
        SCOPED_TRACE("pending snapshot state");
        EXPECT_TRUE(chunk.is_voxel_data_dirty());
        EXPECT_FLOAT_EQ(chunk.heightmap_data.front(), 42.0f);
        EXPECT_EQ(session.ComputePlantSubHash(), plant_hash);
        EXPECT_EQ(session.GetEnergyFieldState()->CanonicalBytes(), energy_bytes);
        EXPECT_EQ(session.GetActiveRegionLedger().canonical_bytes(), ledger_bytes);
        EXPECT_EQ(session.GetWorldClock().canonical_bytes(), clock_bytes);
    }

    const std::string& PlantHash() const {
        return plant_hash;
    }
    const std::string& EnergyBytes() const {
        return energy_bytes;
    }

private:
    const std::string plant_hash;
    const std::string energy_bytes;
    const std::string ledger_bytes;
    const std::string clock_bytes;
};

Server::ServerWorldRunnerConfig SnapshotRunnerConfig(const std::string& root_path,
                                                     bool active_regions) {
    Server::ServerWorldRunnerConfig config;
    config.root_path = root_path;
    config.systems_config_override = luminumbra::core::SystemConfig::FromJsonString(nlohmann::json{
        {"sim",
         {{"active_regions", {{"enabled", active_regions}}},
          {"aether_state", {{"enabled", true}}}}}}.dump());
    config.surface_radius = 0;
    config.collision_radius = 0;
    config.test_streaming_radius_cap = 1;
    config.autosave_interval_ticks = 0;
    return config;
}

void BootSnapshotRunner(Server::ServerWorldRunner& runner, bool active_regions) {
    SCOPED_TRACE("boot snapshot runner");
    ASSERT_TRUE(runner.Boot()) << runner.GetBootError();
    auto* session = runner.Session();
    ASSERT_NE(session, nullptr);
    ASSERT_EQ(session->ActiveRegionsEnabled(), active_regions);
    session->GetWorldSystem()->clear_world(session->GetPhysicsSystem());
}

struct ServerWaterHashes {
    std::string world, chunk, checkpoint;
    Persistence::WorldStreamingStateSubHashes sub, checkpoint_sub;
};

ServerWaterHashes ReadServerWaterHashes(Server::ServerWorldRunner& runner) {
    ServerWaterHashes hashes;
    hashes.world = runner.ComputeWorldHash();
    hashes.sub = runner.ComputeWorldSubHashes();
    runner.ComputeWorldHashAndSubHashes(hashes.checkpoint, hashes.checkpoint_sub);
    hashes.chunk = runner.Session()->ComputeChunkStateHash();
    return hashes;
}

void ExpectSameServerSimulationSubHashes(
    const Persistence::WorldStreamingStateSubHashes& actual,
    const Persistence::WorldStreamingStateSubHashes& expected) {
    EXPECT_EQ(actual.terrain, expected.terrain);
    EXPECT_EQ(actual.water, expected.water);
    EXPECT_EQ(actual.entities, expected.entities);
    EXPECT_EQ(actual.wind, expected.wind);
    EXPECT_EQ(actual.weather, expected.weather);
    EXPECT_EQ(actual.aether, expected.aether);
    EXPECT_EQ(actual.aether_state, expected.aether_state);
}

void ExpectSameServerSubHashes(const Persistence::WorldStreamingStateSubHashes& actual,
                               const Persistence::WorldStreamingStateSubHashes& expected) {
    ExpectSameServerSimulationSubHashes(actual, expected);
    EXPECT_EQ(actual.mesh, expected.mesh);
}

void ExpectSameServerSimulationHashes(const ServerWaterHashes& actual,
                                      const ServerWaterHashes& expected) {
    EXPECT_EQ(actual.world, expected.world);
    EXPECT_EQ(actual.chunk, expected.chunk);
    EXPECT_EQ(actual.checkpoint, expected.checkpoint);
    EXPECT_EQ(actual.checkpoint, actual.world);
    EXPECT_EQ(expected.checkpoint, expected.world);
    ExpectSameServerSimulationSubHashes(actual.sub, expected.sub);
    ExpectSameServerSimulationSubHashes(actual.checkpoint_sub, expected.checkpoint_sub);
    // Mesh versions and derivative geometry are diagnostic, and load invalidates
    // their producers. Pin each run's diagnostic to its own paired capture; all
    // simulation slots above must still match across the process boundary.
    ExpectSameServerSubHashes(actual.checkpoint_sub, actual.sub);
    ExpectSameServerSubHashes(expected.checkpoint_sub, expected.sub);
}

void ExpectSameServerWaterHashes(const ServerWaterHashes& actual,
                                 const ServerWaterHashes& expected) {
    EXPECT_EQ(actual.world, expected.world);
    EXPECT_EQ(actual.chunk, expected.chunk);
    EXPECT_EQ(actual.checkpoint, expected.checkpoint);
    EXPECT_EQ(actual.checkpoint, actual.world);
    ExpectSameServerSubHashes(actual.sub, expected.sub);
    ExpectSameServerSubHashes(actual.checkpoint_sub, expected.checkpoint_sub);
    ExpectSameServerSubHashes(actual.checkpoint_sub, actual.sub);
}

std::shared_ptr<Chunk> PrepareServerWaterChange(GameSession& session) {
    const int resolution = session.GetWorldSystem()->debug_water_sim_resolution();
    const auto side = static_cast<std::size_t>(resolution);
    const auto cells = side * side;
    auto chunk = std::make_shared<Chunk>(IVec3(0, 0, 0));
    chunk->set_state(ChunkState::Idle);
    chunk->sdf_data.assign(std::size_t{17} * 17 * 17, -4.0f);
    chunk->mark_sdf_generated_current_params();
    chunk->current_water_resolution.store(resolution);
    chunk->water_depth_mm.assign(cells, 77);
    chunk->water_bed_mm.assign(cells, 0);
    chunk->water_edge_flux.assign(2 * cells, 0);
    chunk->water_level_data.assign(cells, 0.077f);
    chunk->water_flow_data.assign(cells, Vec2(0.0f));
    chunk->water_sim_terrain_height.assign(cells, 0.0f);
    chunk->water_rest_level.assign(cells, 0.077f);
    chunk->has_water_sim.store(true);
    EXPECT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
    return chunk;
}

void TickServerWithWaterChange(Server::ServerWorldRunner& runner,
                               const std::shared_ptr<Chunk>& chunk) {
    auto* session = runner.Session();
    struct PromotionObservation {
        std::vector<std::int32_t> depth, flux;
        std::size_t count = 0;
    };
    const auto observed = std::make_shared<PromotionObservation>();
    session->GetWorldSystem()->SetWaterAuthorityObserver([observed, chunk](const IVec3& coords) {
        if (coords == chunk->get_coords()) {
            ++observed->count;
            observed->depth = chunk->water_depth_mm;
            observed->flux = chunk->water_edge_flux;
        }
    });
    session->GetSimulationEventBus().subscribe([chunk](const auto&) {
        EXPECT_FALSE(chunk->has_water_authority());
        chunk->water_depth_mm.front() += 1000;
        chunk->mark_water_state_dirty();
    });
    session->GetSimulationEventBus().publish(1, "water.change", "");
    const auto report = runner.RunFixedTicks(1);
    session->GetWorldSystem()->SetWaterAuthorityObserver({});
    ASSERT_EQ(report.ticks_executed, 1u);
    EXPECT_GT(session->GetWaterSystem()->cells_stepped_last_update(), 0u);
    EXPECT_EQ(observed->count, 1u);
    EXPECT_EQ(observed->depth, chunk->water_depth_mm);
    EXPECT_EQ(observed->flux, chunk->water_edge_flux);
    EXPECT_TRUE(chunk->has_water_authority());
    EXPECT_FALSE(chunk->is_water_state_dirty());
}

TEST_F(WorldOpenRefusal, ServerWaterHashesAreIndependentOfSaveTimingWithoutRegions) {
    for (const bool full_snapshot : {false, true}) {
        SCOPED_TRACE(full_snapshot);
        Server::ServerWorldRunner runner(SnapshotRunnerConfig(RootString(), false));
        ASSERT_NO_FATAL_FAILURE(BootSnapshotRunner(runner, false));
        auto* session = runner.Session();
        const auto chunk = PrepareServerWaterChange(*session);
        ASSERT_NO_FATAL_FAILURE(TickServerWithWaterChange(runner, chunk));
        const auto inputs = session->GetWorldSystem()->water_authority_input_items();
        const auto before = ReadServerWaterHashes(runner);
        ExpectSameServerWaterHashes(ReadServerWaterHashes(runner), before);
        if (full_snapshot) {
            ASSERT_GT(runner.SaveFullSnapshot(), 0u);
        } else {
            ASSERT_TRUE(session->SaveWorldState());
        }
        ExpectSameServerWaterHashes(ReadServerWaterHashes(runner), before);
        // Hashes and saves must find the water queue already drained by the host tick.
        EXPECT_EQ(session->GetWorldSystem()->water_authority_input_items(), inputs);
    }
}

void ExpectCaptureSubHashes(const Persistence::WorldStreamingStateSubHashes& actual,
                            const Persistence::WorldStreamingStateSubHashes& expected) {
    EXPECT_EQ(actual.terrain, expected.terrain);
    EXPECT_EQ(actual.mesh, expected.mesh);
    EXPECT_EQ(actual.water, expected.water);
    EXPECT_EQ(actual.entities, expected.entities);
    EXPECT_EQ(actual.wind, expected.wind);
    EXPECT_EQ(actual.weather, expected.weather);
    EXPECT_EQ(actual.aether, expected.aether);
    EXPECT_EQ(actual.aether_state, expected.aether_state);
}

TEST(AuthorityHashCapture, PreparationKeepsLegacyPrecedenceAndRegionalFolds) {
    const auto make_owner = [](int x, float height, ChunkState lifecycle) {
        auto chunk = std::make_shared<Chunk>(IVec3(x, 0, 0));
        chunk->heightmap_data = {height};
        chunk->mark_voxel_data_dirty();
        chunk->set_state(lifecycle);
        return chunk;
    };
    const auto live = make_owner(0, 1.0f, ChunkState::Idle);
    const auto stale = make_owner(0, 9.0f, ChunkState::Idle);
    const auto parked = make_owner(1, 2.0f, ChunkState::Idle);
    const auto loading = make_owner(1, 8.0f, ChunkState::Loading);
    const auto pristine = std::make_shared<Chunk>(IVec3(2, 0, 0));
    const auto outside = make_owner(32, 7.0f, ChunkState::Idle);
    Persistence::RegionDigestInputs inputs{.live = {nullptr, outside, pristine, loading, live},
                                           .parked = {stale, parked},
                                           .durable = {},
                                           .require_index = false,
                                           .ground_object_bytes = "ground",
                                           .field_page_bytes = "pages"};
    WorldStreamingState expected;
    ASSERT_TRUE(expected.insert_chunk(live));
    ASSERT_TRUE(expected.insert_chunk(parked));
    auto legacy = Persistence::ComputeRegionContentHash(expected);
    legacy = Persistence::StableChecksum(legacy + "|ground:" + inputs.ground_object_bytes);
    legacy = Persistence::StableChecksum(legacy + "|pages:" + inputs.field_page_bytes);
    const auto expected_digest = std::stoull(legacy, nullptr, 16);
    Persistence::RegionDigestOperation operation({}, false);
    const auto prepared = operation.prepare({0, 0}, inputs);
    ASSERT_EQ(prepared.chunks.size(), 2u);
    EXPECT_EQ(prepared.chunks[0], live);
    EXPECT_EQ(prepared.chunks[1], parked);
    EXPECT_EQ(Persistence::HashPreparedRegionDigest(prepared), expected_digest);
    EXPECT_EQ(operation.digest({0, 0}, inputs), expected_digest);
    EXPECT_EQ(WorldSaveService::region_simulation_digest({}, {0, 0}, inputs), expected_digest);
    inputs.ground_object_bytes = "changed";
    inputs.field_page_bytes.clear();
    // Prepared bytes retain the observation even if the caller reuses its input buffer.
    EXPECT_EQ(Persistence::HashPreparedRegionDigest(prepared), expected_digest);
}

void ExpectSelectedCaptureParts(const WorldStreamingState& state,
                                std::uint64_t cursor,
                                const std::optional<std::string>& projection,
                                const Persistence::AuthorityRuntimeHashes& runtime,
                                const Persistence::AuthorityHashCapture& capture) {
    namespace P = Persistence;
    for (const auto part : {P::AuthorityCaptureParts::Chunk,
                            P::AuthorityCaptureParts::Composite,
                            P::AuthorityCaptureParts::SubHashes,
                            P::AuthorityCaptureParts::StreamedAndSubHashes}) {
        const auto selected =
            P::CaptureAuthorityHashes(state, cursor, projection, "entities", runtime, part);
        ASSERT_EQ(selected.status, P::AuthorityCaptureStatus::Ready);
        ASSERT_TRUE(selected.capture.has_value());
        if (!selected.capture) {
            return;
        }
        const bool chunk_slot =
            part == P::AuthorityCaptureParts::Chunk || part == P::AuthorityCaptureParts::Composite;
        const bool sub_slot = part == P::AuthorityCaptureParts::SubHashes ||
                              part == P::AuthorityCaptureParts::StreamedAndSubHashes;
        EXPECT_EQ(selected.capture->streamed,
                  part == P::AuthorityCaptureParts::SubHashes ? "" : capture.streamed);
        EXPECT_EQ(selected.capture->chunk, chunk_slot ? capture.chunk : "");
        EXPECT_EQ(selected.capture->composite,
                  part == P::AuthorityCaptureParts::Composite ? capture.composite : "");
        ExpectCaptureSubHashes(selected.capture->sub,
                               sub_slot ? capture.sub : P::WorldStreamingStateSubHashes{});
    }
}

TEST(AuthorityHashCapture, PinsEveryLegacyOutputSlotAtZeroAndNonzeroCursor) {
    namespace P = Persistence;
    WorldStreamingState state;
    const P::AuthorityRuntimeHashes runtime{.wind = "wind",
                                            .weather = "weather",
                                            .aether = "aether",
                                            .scent = "scent",
                                            .ecology = "ecology",
                                            .plant = "plant",
                                            .aether_state = "aether_state"};
    struct Golden {
        std::uint64_t cursor;
        const char* streamed;
        const char* regional;
        const char* composite_off;
        const char* composite_on;
        const char* water;
    };
    const std::array goldens{Golden{0,
                                    "6301549f9c94d118",
                                    "29b901a491f1be19",
                                    "d967df6330fef550",
                                    "4fe511ab409af347",
                                    "da9fb1d4a463ef7d"},
                             Golden{7,
                                    "8ca2f12decbb595c",
                                    "29bc87a491f4d7a2",
                                    "cb223da985a6d7be",
                                    "d9afe0524f020fa9",
                                    "da9fb1d4a463ef7d|waterSimCursor:v1:bd9e024ffae20e07"}};
    for (const auto& golden : goldens) {
        for (const bool regions : {false, true}) {
            SCOPED_TRACE(regions);
            SCOPED_TRACE(golden.cursor);
            const std::optional<std::string> projection =
                regions ? std::optional("authoritative_chunks:v2:" + std::to_string(golden.cursor) +
                                        ":")
                        : std::nullopt;
            const auto result =
                P::CaptureAuthorityHashes(state, golden.cursor, projection, "entities", runtime);
            ASSERT_EQ(result.status, P::AuthorityCaptureStatus::Ready);
            ASSERT_TRUE(result.capture.has_value());
            if (!result.capture) {
                return;
            }
            const auto& capture = result.capture.value();
            EXPECT_EQ(capture.streamed, golden.streamed);
            EXPECT_EQ(capture.streamed, P::ComputeWorldStreamingStateHash(state, golden.cursor));
            EXPECT_EQ(capture.streamed, WorldSaveService{}.world_hash(state, golden.cursor));
            EXPECT_EQ(capture.chunk, regions ? golden.regional : golden.streamed);
            EXPECT_EQ(capture.composite, regions ? golden.composite_on : golden.composite_off);
            EXPECT_EQ(capture.chunk,
                      regions ? P::StableChecksum(projection.value_or(""))
                              : P::ComputeWorldStreamingStateHash(state, golden.cursor));
            EXPECT_EQ(capture.composite,
                      P::ComposeWorldHash(capture.chunk,
                                          runtime.wind,
                                          runtime.weather,
                                          runtime.aether,
                                          runtime.scent,
                                          runtime.ecology,
                                          runtime.plant));
            const P::AuthorityRegionalInputs owners{
                .save_dir = {}, .require_index = false, .regions = {}};
            const auto owned =
                P::CaptureAuthorityHashes(state, golden.cursor, owners, "entities", runtime);
            ASSERT_TRUE(owned.capture.has_value());
            if (!owned.capture) {
                return;
            }
            EXPECT_EQ(owned.capture->chunk, golden.regional);
            EXPECT_EQ(owned.capture->composite, golden.composite_on);
            ASSERT_NO_FATAL_FAILURE(
                ExpectSelectedCaptureParts(state, golden.cursor, projection, runtime, capture));
            EXPECT_EQ(capture.sub.terrain, "7584c0f6de3e387b");
            EXPECT_EQ(capture.sub.mesh, "594db6ffa2560b29");
            EXPECT_EQ(capture.sub.water, golden.water);
            EXPECT_EQ(capture.sub.entities, "935d0fb07bb3822a");
            EXPECT_EQ(capture.sub.wind, "wind");
            EXPECT_EQ(capture.sub.weather, "weather");
            EXPECT_EQ(capture.sub.aether, "aether");
            EXPECT_EQ(capture.sub.aether_state, "aether_state");
            // Independent pre-capture projections remain the sub-slot oracle.
            const auto legacy_sub =
                P::ComputeWorldStreamingStateSubHashes(state, "entities", golden.cursor);
            EXPECT_EQ(capture.sub.terrain, legacy_sub.terrain);
            EXPECT_EQ(capture.sub.mesh, legacy_sub.mesh);
            EXPECT_EQ(capture.sub.water, legacy_sub.water);
            EXPECT_EQ(capture.sub.entities, legacy_sub.entities);
        }
    }
    const auto missing = P::CaptureAuthorityHashes(state, std::nullopt);
    EXPECT_EQ(missing.status, P::AuthorityCaptureStatus::MissingCursor);
    EXPECT_FALSE(missing.capture.has_value());
}

TEST(AuthorityHashCapture, RuntimeEntityCursorAndRegionalInputsSelectTheirLegacySlots) {
    namespace P = Persistence;
    WorldStreamingState state;
    const P::AuthorityRuntimeHashes runtime{.wind = "wind",
                                            .weather = "weather",
                                            .aether = "aether",
                                            .scent = "scent",
                                            .ecology = "ecology",
                                            .plant = "plant",
                                            .aether_state = "state"};
    const std::array members{&P::AuthorityRuntimeHashes::wind,
                             &P::AuthorityRuntimeHashes::weather,
                             &P::AuthorityRuntimeHashes::aether,
                             &P::AuthorityRuntimeHashes::scent,
                             &P::AuthorityRuntimeHashes::ecology,
                             &P::AuthorityRuntimeHashes::plant,
                             &P::AuthorityRuntimeHashes::aether_state};
    for (const bool regions : {false, true}) {
        SCOPED_TRACE(regions);
        const std::optional<std::string> projection =
            regions ? std::optional<std::string>{"authoritative_chunks:v2:0:"} : std::nullopt;
        const auto baseline = P::CaptureAuthorityHashes(state, 0, projection, "entities", runtime);
        ASSERT_TRUE(baseline.capture.has_value());
        if (!baseline.capture) {
            return;
        }
        const auto& before = baseline.capture.value();
        for (const auto member : members) {
            auto changed = runtime;
            changed.*member += "changed";
            const auto result =
                P::CaptureAuthorityHashes(state, 0, projection, "entities", changed);
            ASSERT_TRUE(result.capture.has_value());
            if (!result.capture) {
                return;
            }
            const auto& after = result.capture.value();
            EXPECT_EQ(after.streamed, before.streamed);
            EXPECT_EQ(after.chunk, before.chunk);
            EXPECT_EQ(after.composite == before.composite,
                      member == &P::AuthorityRuntimeHashes::aether_state);
            auto expected = before.sub;
            if (member == &P::AuthorityRuntimeHashes::wind) {
                expected.wind = changed.wind;
            } else if (member == &P::AuthorityRuntimeHashes::weather) {
                expected.weather = changed.weather;
            } else if (member == &P::AuthorityRuntimeHashes::aether) {
                expected.aether = changed.aether;
            } else if (member == &P::AuthorityRuntimeHashes::aether_state) {
                expected.aether_state = changed.aether_state;
            }
            ExpectCaptureSubHashes(after.sub, expected);
        }
        for (const std::string& entities : {std::string{}, std::string{"changed"}}) {
            const auto result = P::CaptureAuthorityHashes(state, 0, projection, entities, runtime);
            ASSERT_TRUE(result.capture.has_value());
            if (!result.capture) {
                return;
            }
            EXPECT_EQ(result.capture->streamed, before.streamed);
            EXPECT_EQ(result.capture->chunk, before.chunk);
            EXPECT_EQ(result.capture->composite, before.composite);
            auto expected = before.sub;
            expected.entities = entities.empty() ? "" : P::StableChecksum(entities);
            ExpectCaptureSubHashes(result.capture->sub, expected);
            EXPECT_NE(result.capture->sub.entities, before.sub.entities);
        }
        const auto next_projection =
            regions ? std::optional<std::string>{"authoritative_chunks:v2:1:"} : std::nullopt;
        const auto next = P::CaptureAuthorityHashes(state, 1, next_projection, "entities", runtime);
        ASSERT_TRUE(next.capture.has_value());
        if (!next.capture) {
            return;
        }
        EXPECT_NE(next.capture->streamed, before.streamed);
        EXPECT_NE(next.capture->chunk, before.chunk);
        EXPECT_NE(next.capture->composite, before.composite);
        EXPECT_NE(next.capture->sub.water, before.sub.water);
        EXPECT_EQ(next.capture->sub.terrain, before.sub.terrain);
        EXPECT_EQ(next.capture->sub.mesh, before.sub.mesh);
    }
    P::AuthorityRegionalInputs regional{.save_dir = {}, .require_index = false, .regions = {}};
    regional.regions.push_back({.key = {0, 0}, .inputs = {}, .frozen_digest = std::nullopt});
    const auto baseline = P::CaptureAuthorityHashes(state, 0, regional);
    ASSERT_TRUE(baseline.capture.has_value());
    if (!baseline.capture) {
        return;
    }
    for (int input = 0; input < 4; ++input) {
        auto changed = regional;
        if (input == 0) {
            changed.regions.front().inputs.ground_object_bytes = "ground";
        } else if (input == 1) {
            changed.regions.front().inputs.field_page_bytes = "pages";
        } else if (input == 2) {
            changed.regions.front().frozen_digest = 42;
        } else {
            changed.regions.front().key = {1, 0};
        }
        const auto result = P::CaptureAuthorityHashes(state, 0, changed);
        ASSERT_TRUE(result.capture.has_value());
        if (!result.capture) {
            return;
        }
        EXPECT_NE(result.capture->chunk, baseline.capture->chunk);
        EXPECT_NE(result.capture->composite, baseline.capture->composite);
        EXPECT_EQ(result.capture->streamed, baseline.capture->streamed);
        ExpectCaptureSubHashes(result.capture->sub, baseline.capture->sub);
    }
}

TEST(AuthorityHashCapture, ChunkInputsKeepStreamedRegionalAndDiagnosticDomains) {
    namespace P = Persistence;
    struct Change {
        const char* name;
        void (*apply)(Chunk&);
        bool streamed;
        bool terrain;
        bool mesh;
        bool water;
        bool regional;
    };
    const std::array changes{
        Change{"sdf", [](Chunk& c) { c.sdf_data = {2.0f}; }, true, true, false, false, true},
        Change{"heightmap",
               [](Chunk& c) { c.heightmap_data = {2.0f}; },
               true,
               true,
               false,
               false,
               true},
        Change{"material", [](Chunk& c) { c.material_data = {1}; }, true, true, false, false, true},
        Change{"lifecycle",
               [](Chunk& c) { c.set_state(ChunkState::Ready); },
               true,
               true,
               false,
               false,
               false},
        Change{"collision",
               [](Chunk& c) { c.has_collision.store(true); },
               true,
               false,
               false,
               false,
               false},
        Change{"mesh", [](Chunk& c) { c.mesh_indices = {1}; }, false, false, true, false, false},
        Change{"mesh version",
               [](Chunk& c) { c.mesh_version.store(1); },
               false,
               false,
               true,
               false,
               false},
        Change{"water depth",
               [](Chunk& c) { c.water_depth_mm = {2}; },
               true,
               false,
               false,
               true,
               true},
        Change{"water bed", [](Chunk& c) { c.water_bed_mm = {2}; }, true, false, false, true, true},
        Change{"water flux",
               [](Chunk& c) { c.water_edge_flux = {2}; },
               true,
               false,
               false,
               true,
               true},
        Change{"water initialized",
               [](Chunk& c) { c.has_water_sim.store(true); },
               true,
               false,
               false,
               true,
               true},
        Change{"water resolution",
               [](Chunk& c) { c.current_water_resolution.store(4); },
               true,
               false,
               false,
               true,
               true},
        Change{"water sleeping",
               [](Chunk& c) { c.is_water_sleeping.store(true); },
               true,
               false,
               false,
               true,
               true},
        Change{"water delta",
               [](Chunk& c) { c.max_water_delta_last_tick = 1.0f; },
               true,
               false,
               false,
               true,
               true},
        Change{"water quiet ticks",
               [](Chunk& c) { c.ticks_below_threshold = 1; },
               true,
               false,
               false,
               true,
               true},
        Change{"water mesh",
               [](Chunk& c) { c.water_mesh_generated.store(true); },
               false,
               false,
               true,
               false,
               false},
        Change{"mesh vertices",
               [](Chunk& c) {
                   c.mesh_vertices.push_back({.position = Vec3(1.0f),
                                              .normal = Vec3(0.0f, 1.0f, 0.0f),
                                              .material_id = 1});
               },
               false,
               false,
               true,
               false,
               false},
        Change{"water mesh vertices",
               [](Chunk& c) {
                   c.water_mesh_vertices.push_back({.position = Vec3(1.0f),
                                                    .normal = Vec3(0.0f, 1.0f, 0.0f),
                                                    .material_id = 1});
               },
               false,
               false,
               true,
               false,
               false},
        Change{"water mesh indices",
               [](Chunk& c) { c.water_mesh_indices = {1}; },
               false,
               false,
               true,
               false,
               false},
        Change{"pending mesh vertices",
               [](Chunk& c) {
                   c.pending_mesh_vertices.push_back({.position = Vec3(1.0f),
                                                      .normal = Vec3(0.0f, 1.0f, 0.0f),
                                                      .material_id = 1});
               },
               false,
               false,
               true,
               false,
               false},
        Change{"pending mesh indices",
               [](Chunk& c) { c.pending_mesh_indices = {1}; },
               false,
               false,
               true,
               false,
               false},
        Change{"pending water vertices",
               [](Chunk& c) {
                   c.pending_water_mesh_vertices.push_back({.position = Vec3(1.0f),
                                                            .normal = Vec3(0.0f, 1.0f, 0.0f),
                                                            .material_id = 1});
               },
               false,
               false,
               true,
               false,
               false},
        Change{"pending water indices",
               [](Chunk& c) { c.pending_water_mesh_indices = {1}; },
               false,
               false,
               true,
               false,
               false},
        Change{"water mesh version",
               [](Chunk& c) { c.water_mesh_version.store(1); },
               false,
               false,
               true,
               false,
               false},
        Change{"water mesh ticks",
               [](Chunk& c) { c.water_mesh_dirty_ticks = 1; },
               false,
               false,
               true,
               false,
               false},
        Change{"mesh ready",
               [](Chunk& c) { c.pending_mesh_ready.store(true); },
               false,
               false,
               false,
               false,
               false},
        Change{"mesh failed",
               [](Chunk& c) { c.pending_mesh_failed.store(true); },
               false,
               false,
               false,
               false,
               false},
        Change{"lod", [](Chunk& c) { c.current_lod.store(1); }, false, false, false, false, false},
        Change{"pending lod",
               [](Chunk& c) { c.pending_lod.store(1); },
               false,
               false,
               false,
               false,
               false},
        Change{"water mirror",
               [](Chunk& c) { c.water_level_data = {1.0f}; },
               false,
               false,
               false,
               false,
               false},
        Change{"flow mirror",
               [](Chunk& c) { c.water_flow_data = {Vec2(1.0f, 0.0f)}; },
               false,
               false,
               false,
               false,
               false},
        Change{"water terrain mirror",
               [](Chunk& c) { c.water_sim_terrain_height = {1.0f}; },
               false,
               false,
               false,
               false,
               false},
        Change{"water ownership",
               [](Chunk& c) { c.mark_water_data_dirty(); },
               true,
               false,
               false,
               false,
               false},
        Change{"generated terrain",
               [](Chunk& c) { c.mark_sdf_generated_current_params(); },
               false,
               false,
               false,
               false,
               true}};

    for (const auto& change : changes) {
        SCOPED_TRACE(change.name);
        for (const bool regions : {false, true}) {
            WorldStreamingState state;
            const auto chunk = state.get_or_create_chunk(IVec3(0, 0, 0));
            chunk->set_state(ChunkState::Idle);
            chunk->mark_voxel_data_dirty();
            P::AuthorityRegionalInputs regional{
                .save_dir = {}, .require_index = false, .regions = {}};
            regional.regions.push_back({.key = {0, 0},
                                        .inputs = {.live = {chunk}, .parked = {}, .durable = {}},
                                        .frozen_digest = std::nullopt});
            const auto capture = [&] {
                return regions ? P::CaptureAuthorityHashes(state, 0, regional)
                               : P::CaptureAuthorityHashes(state, 0);
            };
            const auto before = capture();
            ASSERT_TRUE(before.capture.has_value());
            if (!before.capture) {
                return;
            }
            change.apply(*chunk);
            const auto after = capture();
            ASSERT_TRUE(after.capture.has_value());
            if (!after.capture) {
                return;
            }
            EXPECT_EQ(after.capture->streamed != before.capture->streamed, change.streamed);
            EXPECT_EQ(after.capture->chunk != before.capture->chunk,
                      regions ? change.regional : change.streamed);
            EXPECT_EQ(after.capture->composite != before.capture->composite,
                      regions ? change.regional : change.streamed);
            EXPECT_EQ(after.capture->sub.terrain != before.capture->sub.terrain, change.terrain);
            EXPECT_EQ(after.capture->sub.mesh != before.capture->sub.mesh, change.mesh);
            EXPECT_EQ(after.capture->sub.water != before.capture->sub.water, change.water);
            const auto legacy = P::ComputeWorldStreamingStateHash(state);
            EXPECT_EQ(after.capture->streamed, legacy);
            ExpectCaptureSubHashes(after.capture->sub,
                                   P::ComputeWorldStreamingStateSubHashes(state));
            if (regions) {
                const auto digest = WorldSaveService::region_simulation_digest(
                    {}, {0, 0}, regional.regions.front().inputs);
                EXPECT_EQ(after.capture->chunk,
                          P::StableChecksum(
                              "authoritative_chunks:v2:0:0,0:" + std::to_string(digest) + ";"));
            } else {
                EXPECT_EQ(after.capture->chunk, legacy);
            }
        }
    }
}

TEST(AuthorityHashCapture, ChunkIdentityFeedsEveryLegacySlot) {
    namespace P = Persistence;
    // Chunk coordinates and the derived chunk id feed every chunk diagnostic group.
    WorldStreamingState original, relocated;
    const auto left = original.get_or_create_chunk(IVec3(0, 0, 0));
    const auto right = relocated.get_or_create_chunk(IVec3(1, 0, 0));
    left->set_state(ChunkState::Idle);
    right->set_state(ChunkState::Idle);
    left->mark_voxel_data_dirty();
    right->mark_voxel_data_dirty();
    for (const bool regions : {false, true}) {
        const P::AuthorityRegionalInputs left_region{
            .save_dir = {},
            .require_index = false,
            .regions = {{.key = {0, 0},
                         .inputs = {.live = {left}, .parked = {}, .durable = {}},
                         .frozen_digest = std::nullopt}}};
        const P::AuthorityRegionalInputs right_region{
            .save_dir = {},
            .require_index = false,
            .regions = {{.key = {0, 0},
                         .inputs = {.live = {right}, .parked = {}, .durable = {}},
                         .frozen_digest = std::nullopt}}};
        const auto before = regions ? P::CaptureAuthorityHashes(original, 0, left_region)
                                    : P::CaptureAuthorityHashes(original, 0);
        const auto after = regions ? P::CaptureAuthorityHashes(relocated, 0, right_region)
                                   : P::CaptureAuthorityHashes(relocated, 0);
        ASSERT_TRUE(before.capture.has_value());
        ASSERT_TRUE(after.capture.has_value());
        if (!before.capture || !after.capture) {
            return;
        }
        EXPECT_NE(after.capture->streamed, before.capture->streamed);
        EXPECT_NE(after.capture->chunk, before.capture->chunk);
        EXPECT_NE(after.capture->composite, before.capture->composite);
        EXPECT_NE(after.capture->sub.terrain, before.capture->sub.terrain);
        EXPECT_NE(after.capture->sub.mesh, before.capture->sub.mesh);
        EXPECT_NE(after.capture->sub.water, before.capture->sub.water);
    }
}

TEST_F(WorldOpenRefusal, AuthorityCaptureSkipsRegionalReadsAndNeverPublishesRefusals) {
    namespace P = Persistence;
    WorldStreamingState state;
    P::AuthorityRegionalInputs regional{.save_dir = save, .require_index = false, .regions = {}};
    regional.regions.push_back(
        {.key = {-1, 0},
         .inputs = {.live = {}, .parked = {}, .durable = {}, .require_index = false},
         .frozen_digest = std::nullopt});
    regional.regions.push_back(
        {.key = {0, 0},
         .inputs = {.live = {}, .parked = {}, .durable = {{0, 0, 0}}, .require_index = false},
         .frozen_digest = std::nullopt});
    const auto baseline = P::CaptureAuthorityHashes(state, 0, regional);
    ASSERT_EQ(baseline.status, P::AuthorityCaptureStatus::Ready);
    ASSERT_TRUE(baseline.capture.has_value());
    if (!baseline.capture) {
        return;
    }
    Write(WorldSaveService::region_file_path(save, 0, 0), "corrupt owner");
    const auto before = DiskBytes(root);
    for (const auto part : {P::AuthorityCaptureParts::All,
                            P::AuthorityCaptureParts::Chunk,
                            P::AuthorityCaptureParts::Composite,
                            P::AuthorityCaptureParts::SubHashes,
                            P::AuthorityCaptureParts::StreamedAndSubHashes}) {
        const auto reads = WorldSaveService::region_read_count_for_testing();
        const auto missing = P::CaptureAuthorityHashes(state, std::nullopt, regional, {}, {}, part);
        EXPECT_EQ(missing.status, P::AuthorityCaptureStatus::MissingCursor);
        EXPECT_FALSE(missing.capture.has_value());
        EXPECT_EQ(WorldSaveService::region_read_count_for_testing(), reads);
        const auto result = P::CaptureAuthorityHashes(state, 0, regional, {}, {}, part);
        const bool skip = part == P::AuthorityCaptureParts::SubHashes ||
                          part == P::AuthorityCaptureParts::StreamedAndSubHashes;
        EXPECT_EQ(result.status,
                  skip ? P::AuthorityCaptureStatus::Ready
                       : P::AuthorityCaptureStatus::PreparationRefused);
        EXPECT_EQ(result.capture.has_value(), skip);
        if (skip) {
            EXPECT_EQ(WorldSaveService::region_read_count_for_testing(), reads);
            ASSERT_TRUE(result.capture.has_value());
            if (!result.capture) {
                return;
            }
            EXPECT_EQ(result.capture->streamed,
                      part == P::AuthorityCaptureParts::StreamedAndSubHashes
                          ? baseline.capture->streamed
                          : "");
            EXPECT_TRUE(result.capture->chunk.empty());
            EXPECT_TRUE(result.capture->composite.empty());
            ExpectCaptureSubHashes(result.capture->sub, baseline.capture->sub);
        }
    }
    // Index admission is also skipped, including for an empty region set.
    const bool had_index = P::snapshot_index_exists(save);
    const auto index_bytes = had_index ? Read(P::snapshot_index_path(save)) : std::string();
    Write(P::snapshot_index_path(save), "{");
    regional.regions.clear();
    EXPECT_EQ(P::CaptureAuthorityHashes(
                  state, 0, regional, {}, {}, P::AuthorityCaptureParts::StreamedAndSubHashes)
                  .status,
              P::AuthorityCaptureStatus::Ready);
    const auto refused = P::CaptureAuthorityHashes(state, 0, regional);
    EXPECT_EQ(refused.status, P::AuthorityCaptureStatus::PreparationRefused);
    EXPECT_FALSE(refused.capture.has_value());
    if (had_index) {
        Write(P::snapshot_index_path(save), index_bytes);
    } else {
        fs::remove(P::snapshot_index_path(save));
    }
    EXPECT_EQ(DiskBytes(root), before);
    EXPECT_STREQ(P::AuthorityCaptureRefused{}.what(), "authority capture refused");
}

TEST_F(WorldOpenRefusal, AuthorityCaptureRegionalIndexOptOutGovernsEveryRegion) {
    namespace P = Persistence;
    WorldStreamingState state;
    // A legacy save: no snapshot index until its first complete indexed save.
    const bool had_index = P::snapshot_index_exists(save);
    const auto index_bytes = had_index ? Read(P::snapshot_index_path(save)) : std::string();
    if (had_index) {
        ASSERT_TRUE(fs::remove(P::snapshot_index_path(save)));
    }
    const auto capture = [&](bool region_require_index) {
        P::AuthorityRegionalInputs regional{
            .save_dir = save, .require_index = false, .regions = {}};
        regional.regions.push_back({.key = {0, 0},
                                    .inputs = {.live = {},
                                               .parked = {},
                                               .durable = {{0, 0, 0}},
                                               .require_index = region_require_index},
                                    .frozen_digest = std::nullopt});
        return P::CaptureAuthorityHashes(state, 0, regional);
    };
    const auto opted_out = capture(false);
    // The per-region default (true) must not override the caller's regional opt-out.
    const auto defaulted = capture(true);
    if (had_index) {
        Write(P::snapshot_index_path(save), index_bytes);
    }
    ASSERT_EQ(opted_out.status, P::AuthorityCaptureStatus::Ready);
    EXPECT_EQ(defaulted.status, P::AuthorityCaptureStatus::Ready);
    ASSERT_TRUE(opted_out.capture.has_value());
    ASSERT_TRUE(defaulted.capture.has_value());
    if (!opted_out.capture || !defaulted.capture) {
        return;
    }
    EXPECT_EQ(defaulted.capture->chunk, opted_out.capture->chunk);
    EXPECT_EQ(defaulted.capture->composite, opted_out.capture->composite);
}

TEST(AuthorityHashCapture, IncompleteLiveAndParkedOwnersRefuseBeforePublication) {
    namespace P = Persistence;
    const auto owner = std::make_shared<Chunk>(IVec3(0, 0, 0));
    owner->mark_voxel_data_dirty();
    owner->set_state(ChunkState::Loading);
    WorldStreamingState empty, streamed;
    ASSERT_TRUE(streamed.insert_chunk(owner));
    for (const auto part : {P::AuthorityCaptureParts::All,
                            P::AuthorityCaptureParts::Chunk,
                            P::AuthorityCaptureParts::Composite,
                            P::AuthorityCaptureParts::SubHashes,
                            P::AuthorityCaptureParts::StreamedAndSubHashes}) {
        const auto result = P::CaptureAuthorityHashes(streamed, 0, std::nullopt, {}, {}, part);
        EXPECT_EQ(result.status, P::AuthorityCaptureStatus::IncompleteOwner);
        EXPECT_FALSE(result.capture.has_value());
        for (const bool parked : {false, true}) {
            const P::AuthorityRegionalInputs regional{
                .save_dir = {},
                .require_index = false,
                .regions = {{.key = {0, 0},
                             .inputs = {.live = parked ? std::vector<std::shared_ptr<Chunk>>{}
                                                       : std::vector{owner},
                                        .parked = parked ? std::vector{owner}
                                                         : std::vector<std::shared_ptr<Chunk>>{},
                                        .durable = {}},
                             .frozen_digest = std::nullopt}}};
            const auto regional_result =
                P::CaptureAuthorityHashes(empty, 0, regional, {}, {}, part);
            const bool skip = part == P::AuthorityCaptureParts::SubHashes ||
                              part == P::AuthorityCaptureParts::StreamedAndSubHashes;
            EXPECT_EQ(regional_result.status,
                      skip ? P::AuthorityCaptureStatus::Ready
                           : P::AuthorityCaptureStatus::IncompleteOwner);
            EXPECT_EQ(regional_result.capture.has_value(), skip);
        }
    }
    EXPECT_EQ(owner->get_state(), ChunkState::Loading);
    EXPECT_EQ(streamed.find_chunk(owner->get_coords()), owner);
    owner->set_state(ChunkState::Idle);
    EXPECT_EQ(P::CaptureAuthorityHashes(streamed, 0).status, P::AuthorityCaptureStatus::Ready);
    const auto pristine = empty.get_or_create_chunk(IVec3(1, 0, 0));
    pristine->set_state(ChunkState::Loading);
    EXPECT_EQ(P::CaptureAuthorityHashes(empty, 0).status, P::AuthorityCaptureStatus::Ready);
}

TEST(AuthorityHashCapture, InvalidRegionalOrderKeysAndPartsRefuseWithoutCapture) {
    namespace P = Persistence;
    WorldStreamingState state;
    for (const int invalid : {0, 1, 2, 3, 4, 5}) {
        P::AuthorityRegionalInputs regional{
            .save_dir = {},
            .require_index = false,
            .regions = {{.key = {0, 0}, .inputs = {}, .frozen_digest = 0},
                        {.key = {1, 0}, .inputs = {}, .frozen_digest = 0}}};
        if (invalid == 0) {
            regional.regions.back().key = {0, 0};
        } else if (invalid == 1) {
            std::reverse(regional.regions.begin(), regional.regions.end());
        } else if (invalid == 2) {
            regional.regions.back().key = {std::numeric_limits<int>::max(), 0};
        } else if (invalid == 3) {
            regional.regions.back().key = {std::numeric_limits<int>::min(), 0};
        } else if (invalid == 4) {
            regional.regions.back().key = {1, std::numeric_limits<int>::max()};
        } else {
            regional.regions.back().key = {1, std::numeric_limits<int>::min()};
        }
        const auto result = P::CaptureAuthorityHashes(state, 0, regional);
        EXPECT_EQ(result.status, P::AuthorityCaptureStatus::PreparationRefused);
        EXPECT_FALSE(result.capture.has_value());
    }
    P::AuthorityRegionalInputs ordered{
        .save_dir = {},
        .require_index = false,
        .regions = {{.key = {0, -1}, .inputs = {}, .frozen_digest = 0},
                    {.key = {0, 0}, .inputs = {}, .frozen_digest = 0}}};
    const auto valid = P::CaptureAuthorityHashes(state, 0, ordered);
    ASSERT_TRUE(valid.capture.has_value());
    if (!valid.capture) {
        return;
    }
    EXPECT_EQ(valid.capture->chunk, P::StableChecksum("authoritative_chunks:v2:0:0,-1:0;0,0:0;"));
    std::reverse(ordered.regions.begin(), ordered.regions.end());
    const auto reversed = P::CaptureAuthorityHashes(state, 0, ordered);
    EXPECT_EQ(reversed.status, P::AuthorityCaptureStatus::PreparationRefused);
    EXPECT_FALSE(reversed.capture.has_value());
    for (const bool owners : {false, true}) {
        const auto invalid = static_cast<P::AuthorityCaptureParts>(-1);
        const auto result =
            owners
                ? P::CaptureAuthorityHashes(state, 0, P::AuthorityRegionalInputs{}, {}, {}, invalid)
                : P::CaptureAuthorityHashes(state, 0, std::nullopt, {}, {}, invalid);
        EXPECT_EQ(result.status, P::AuthorityCaptureStatus::PreparationRefused);
        EXPECT_FALSE(result.capture.has_value());
    }
}

void CheckSessionCheckpointPublication(const fs::path& root,
                                       const Persistence::AuthorityHashCapture& expected_capture) {
    Net::LockstepConfig config;
    config.hash_cadence_ticks = 1;
    config.system_config_identity = "config";
    config.preset_content_hash = "content";
    auto remote = config;
    remote.local_client_id = 1;
    remote.peer_client_id = 0;
    auto [local, peer] = Net::MakeLoopbackPair();
    Net::LockstepHooks hooks;
    // Both publications consume exactly the same immutable session observation.
    struct CaptureContext {
        const Persistence::AuthorityHashCapture& capture;
        std::size_t boot_calls = 0;
        std::size_t tick_calls = 0;
    } context{expected_capture};
    hooks.user = &context;
    hooks.capture_hashes = [](std::uint64_t tick, Net::HashMsg& out, void* user) {
        auto& capture_context = *static_cast<CaptureContext*>(user);
        const auto& capture = capture_context.capture;
        if (tick == 0) {
            ++capture_context.boot_calls;
        } else {
            ++capture_context.tick_calls;
        }
        out = {
            tick, capture.streamed, capture.sub.terrain, capture.sub.water, capture.sub.entities};
    };
    Net::LockstepSession lockstep(config, local.get(), hooks);
    lockstep.SetDumpPath((root / "capture-agreement.lrec1").string());
    ASSERT_TRUE(peer->SendFrame(Net::EncodeHello(Net::MakeLockstepHello(remote))));
    ASSERT_TRUE(lockstep.Handshake());
    EXPECT_EQ(context.boot_calls, 1u);
    EXPECT_TRUE(lockstep.Status().replay_error.empty()) << lockstep.Status().replay_error;
    ASSERT_TRUE(peer->SendFrame(Net::EncodeInput({1, 1, {}})));
    EXPECT_EQ(lockstep.PumpTick(2).outcome, Net::TickOutcome::Advanced);
    EXPECT_EQ(context.tick_calls, 1u);
    std::size_t published = 0;
    std::vector<std::uint8_t> frame;
    // Hello, the initial horizon, the next input and one hash bound the drain.
    for (std::uint32_t i = 0; i < config.horizon_max + 3; ++i) {
        if (!peer->TryReceiveFrame(frame)) {
            break;
        }
        Net::MessageType type{};
        ASSERT_TRUE(Net::PeekMessageType(frame, type));
        if (type == Net::MessageType::Hash) {
            Net::HashMsg hash;
            ASSERT_TRUE(Net::DecodeHash(frame, hash));
            EXPECT_EQ(hash.tick, 1u);
            EXPECT_EQ(hash.world_hash, expected_capture.streamed);
            EXPECT_EQ(hash.terrain, expected_capture.sub.terrain);
            EXPECT_EQ(hash.water, expected_capture.sub.water);
            EXPECT_EQ(hash.entities, expected_capture.sub.entities);
            ++published;
        }
    }
    EXPECT_EQ(published, 1u);
    EXPECT_FALSE(peer->TryReceiveFrame(frame));
    // Diagnostic replays are published on divergence; a clean disconnect only closes transport.
    ASSERT_TRUE(peer->SendFrame(Net::EncodeHash({1,
                                                 expected_capture.streamed + "-different",
                                                 expected_capture.sub.terrain,
                                                 expected_capture.sub.water,
                                                 expected_capture.sub.entities})));
    lockstep.Disconnect();
    EXPECT_TRUE(lockstep.Status().desynced);
    EXPECT_EQ(lockstep.Status().desync_section, "world_hash");
    EXPECT_EQ(lockstep.Status().dump_path, (root / "capture-agreement.lrec1").string());
    const auto recording = Replay::ReadReplay((root / "capture-agreement.lrec1").string());
    ASSERT_TRUE(recording.has_value());
    if (!recording) {
        return;
    }
    EXPECT_EQ(recording->header.start_world_hash, expected_capture.streamed);
    EXPECT_FALSE(recording->truncated);
    EXPECT_TRUE(recording->trailer_present);
    ASSERT_EQ(recording->checkpoints.size(), 1u);
    const auto& checkpoint = recording->checkpoints.front();
    EXPECT_EQ(checkpoint.tick, 1u);
    EXPECT_EQ(checkpoint.world_hash, expected_capture.streamed);
    EXPECT_EQ(checkpoint.terrain, expected_capture.sub.terrain);
    EXPECT_EQ(checkpoint.water, expected_capture.sub.water);
    EXPECT_EQ(checkpoint.entities, expected_capture.sub.entities);
}

TEST_F(WorldOpenRefusal, SessionCaptureAndLockstepCheckpointKeepLegacyBytes) {
    namespace P = Persistence;
    for (const bool regions : {false, true}) {
        Server::ServerWorldRunner runner(SnapshotRunnerConfig(RootString(), regions));
        ASSERT_NO_FATAL_FAILURE(BootSnapshotRunner(runner, regions));
        auto& session = *runner.Session();
        auto chunk = std::make_shared<Chunk>(IVec3(0, 0, 0));
        chunk->heightmap_data = {2.0f};
        chunk->mark_voxel_data_dirty();
        chunk->set_state(ChunkState::Idle);
        ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
        const auto entities =
            Ecs::SerializeEntityRegistrySnapshotJson(Ecs::EntityRegistrySnapshot{});
        for (const std::size_t cursor : {std::size_t{0}, std::size_t{7}}) {
            session.GetWorldSystem()->SetWaterSimWindowCursor(cursor);
            const auto full = session.CaptureAuthorityHashes(entities);
            const auto checkpoint = session.CaptureAuthorityHashes(
                entities, P::AuthorityCaptureParts::StreamedAndSubHashes);
            ASSERT_TRUE(full.capture.has_value());
            ASSERT_TRUE(checkpoint.capture.has_value());
            if (!full.capture || !checkpoint.capture) {
                return;
            }
            EXPECT_EQ(full.status, P::AuthorityCaptureStatus::Ready);
            EXPECT_EQ(checkpoint.status, P::AuthorityCaptureStatus::Ready);
            EXPECT_EQ(full.capture->streamed, checkpoint.capture->streamed);
            ExpectCaptureSubHashes(full.capture->sub, checkpoint.capture->sub);
            EXPECT_EQ(session.ComputeChunkStateHash(), full.capture->chunk);
            EXPECT_EQ(TestSupport::SessionWorldHash(session), full.capture->composite);
            WorldStreamingState state;
            ASSERT_TRUE(state.insert_chunk(chunk));
            EXPECT_EQ(checkpoint.capture->streamed, WorldSaveService{}.world_hash(state, cursor));
            const auto digest = std::stoull(P::ComputeRegionContentHash(state), nullptr, 16);
            const auto legacy_chunk =
                regions ? P::StableChecksum("authoritative_chunks:v2:" + std::to_string(cursor) +
                                            ":0,0:" + std::to_string(digest) + ";")
                        : WorldSaveService{}.world_hash(state, cursor);
            EXPECT_EQ(full.capture->chunk, legacy_chunk);
            const auto legacy = P::ComputeWorldStreamingStateSubHashes(state, entities, cursor);
            EXPECT_EQ(checkpoint.capture->sub.terrain, legacy.terrain);
            EXPECT_EQ(checkpoint.capture->sub.water, legacy.water);
            EXPECT_EQ(checkpoint.capture->sub.entities, legacy.entities);

            ASSERT_NO_FATAL_FAILURE(CheckSessionCheckpointPublication(root, *checkpoint.capture));
        }
    }
}

TEST_F(WorldOpenRefusal, SessionCheckpointSkipsFailedRegionalReadsAndLatchedFaultIsHashable) {
    namespace P = Persistence;
    JobSystem capture_jobs;
    capture_jobs.startup(1);
    GameSession session;
    session.SetJobSystem(&capture_jobs);
    session.SetRootPath(RootString());
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.LoadWorld("fixture")) << session.GetWorldOpenError();
    auto& world = *session.GetWorldSystem();
    world.clear_world(session.GetPhysicsSystem());
    world.remember_durable_chunks({IVec3(0, 0, 0)});
    ASSERT_EQ(world.authority_region_keys(), (std::vector<std::pair<i32, i32>>{{0, 0}}));
    ASSERT_EQ(world.region_digest_inputs(0, 0).durable, (std::vector<IVec3>{{0, 0, 0}}));
    const auto original = session.CaptureAuthorityHashes();
    ASSERT_EQ(original.status, P::AuthorityCaptureStatus::Ready);
    const auto region = WorldSaveService::region_file_path(save, 0, 0);
    const auto bytes = Read(region);
    for (const bool missing : {false, true}) {
        SCOPED_TRACE(missing);
        if (missing) {
            ASSERT_TRUE(fs::remove(region));
        } else {
            Write(region, "partial");
        }
        const auto disk = DiskBytes(save);
        const auto inputs = world.region_digest_input_items();
        const auto checkpoint =
            session.CaptureAuthorityHashes({}, P::AuthorityCaptureParts::StreamedAndSubHashes);
        EXPECT_EQ(checkpoint.status, P::AuthorityCaptureStatus::Ready);
        EXPECT_TRUE(checkpoint.capture.has_value());
        EXPECT_EQ(world.region_digest_input_items(), inputs);
        const auto full = session.CaptureAuthorityHashes();
        EXPECT_EQ(full.status, P::AuthorityCaptureStatus::PreparationRefused);
        EXPECT_FALSE(full.capture.has_value());
        EXPECT_TRUE(session.ComputeChunkStateHash().empty());
        EXPECT_TRUE(session.GetRegionSchedulerFault().has_value());
        EXPECT_EQ(DiskBytes(save), disk);
        Write(region, bytes);
        // Recovery deliberately leaves the scheduler fault latched.
        EXPECT_EQ(session.CaptureAuthorityHashes().status, P::AuthorityCaptureStatus::Ready);
        EXPECT_TRUE(session.GetRegionSchedulerFault().has_value());
    }
    capture_jobs.shutdown();
    const auto unavailable = session.CaptureAuthorityHashes();
    EXPECT_EQ(unavailable.status, P::AuthorityCaptureStatus::PreparationRefused);
    EXPECT_FALSE(unavailable.capture.has_value());
    EXPECT_EQ(
        session.CaptureAuthorityHashes({}, P::AuthorityCaptureParts::StreamedAndSubHashes).status,
        P::AuthorityCaptureStatus::Ready);
    capture_jobs.startup(1);
    EXPECT_EQ(session.CaptureAuthorityHashes().status, P::AuthorityCaptureStatus::Ready);
}

void CheckRequiredRegionalReadRefusals(const fs::path& directory,
                                       Persistence::RegionDigestOperation& operation,
                                       const Persistence::RegionDigestInputs& required,
                                       const Persistence::PreparedRegionDigest& prepared) {
    namespace P = Persistence;
    EXPECT_THROW((void)operation.prepare({0, 0}, required), std::runtime_error);
    for (const bool parked : {false, true}) {
        auto resident = required;
        (parked ? resident.parked : resident.live) = prepared.chunks;
        EXPECT_THROW((void)operation.prepare({0, 0}, resident), std::runtime_error);
    }
    EXPECT_THROW((void)operation.digest({0, 0}, required), std::runtime_error);
    EXPECT_THROW((void)WorldSaveService::region_simulation_digest(directory, {0, 0}, required),
                 std::runtime_error);
    const P::AuthorityRegionalInputs regional{
        .save_dir = directory,
        .require_index = required.require_index,
        .regions = {{.key = {0, 0}, .inputs = required, .frozen_digest = std::nullopt}}};
    const auto capture = P::CaptureAuthorityHashes(WorldStreamingState{}, 0, regional);
    EXPECT_EQ(capture.status, P::AuthorityCaptureStatus::PreparationRefused);
    EXPECT_FALSE(capture.capture.has_value());
}

void CheckRegionalReadDamage(const fs::path& directory,
                             int damage,
                             Persistence::RegionDigestOperation& operation,
                             const Persistence::RegionDigestInputs& optional,
                             const Persistence::PreparedRegionDigest& prepared) {
    namespace P = Persistence;
    SCOPED_TRACE(damage);
    const auto region = WorldSaveService::region_file_path(directory, 0, 0);
    const auto bytes = Read(region);
    const auto digest = P::HashPreparedRegionDigest(prepared);
    auto required = optional;
    required.durable = {{0, 0, 0}};
    ASSERT_TRUE(fs::remove(region));
    if (damage == 0) {
        Write(region, "partial");
    } else if (damage == 2) {
        ASSERT_TRUE(fs::create_directory(region));
    }
    const auto before = DiskBytes(directory);
    // An optional absent legacy region may be cached as empty. That observation must
    // never satisfy the following required read, including on the same operation.
    if (!optional.require_index && damage == 1) {
        EXPECT_TRUE(operation.prepare({0, 0}, optional).chunks.empty());
    } else {
        EXPECT_THROW((void)operation.prepare({0, 0}, optional), std::runtime_error);
    }
    ASSERT_NO_FATAL_FAILURE(
        CheckRequiredRegionalReadRefusals(directory, operation, required, prepared));
    EXPECT_EQ(DiskBytes(directory), before);
    if (damage == 2) {
        ASSERT_TRUE(fs::remove(region));
    }
    Write(region, bytes);
    EXPECT_EQ(P::HashPreparedRegionDigest(operation.prepare({0, 0}, required)), digest);
    EXPECT_EQ(operation.digest({0, 0}, required), digest);
    const P::AuthorityRegionalInputs regional{
        .save_dir = directory,
        .require_index = required.require_index,
        .regions = {{.key = {0, 0}, .inputs = required, .frozen_digest = std::nullopt}}};
    EXPECT_EQ(P::CaptureAuthorityHashes(WorldStreamingState{}, 0, regional).status,
              P::AuthorityCaptureStatus::Ready);
}

TEST_F(WorldOpenRefusal, RegionalPreparationWithoutSaveRootRejectsDurableParticipants) {
    namespace P = Persistence;
    P::RegionDigestInputs optional{.live = {},
                                   .parked = {},
                                   .durable = {},
                                   .require_index = false,
                                   .ground_object_bytes = {},
                                   .field_page_bytes = {}};
    P::RegionDigestOperation operation({}, false);
    EXPECT_TRUE(operation.prepare({0, 0}, optional).chunks.empty());
    const auto live = std::make_shared<Chunk>(IVec3(0, 0, 0));
    live->heightmap_data = {3.0f};
    live->mark_voxel_data_dirty();
    live->set_state(ChunkState::Idle);
    optional.live = {live};
    const auto prepared = operation.prepare({0, 0}, optional);
    ASSERT_EQ(prepared.chunks.size(), 1u);
    EXPECT_EQ(prepared.chunks.front(), live);
    auto required = optional;
    required.live.clear();
    required.durable = {{0, 0, 0}};
    ASSERT_NO_FATAL_FAILURE(CheckRequiredRegionalReadRefusals({}, operation, required, prepared));
    optional.durable = {{-32, 0, 0}};
    EXPECT_EQ(operation.prepare({0, 0}, optional).chunks, prepared.chunks);
    EXPECT_THROW((void)operation.prepare({-1, 0}, optional), std::runtime_error);
}

TEST_F(WorldOpenRefusal, RegionalPreparationDistinguishesOptionalAndRequiredDurableReads) {
    namespace P = Persistence;
    for (const bool indexed : {false, true}) {
        SCOPED_TRACE(indexed);
        const auto directory = root / (indexed ? "indexed-digest" : "legacy-digest");
        fs::copy(save, directory, fs::copy_options::recursive);
        if (indexed) {
            ASSERT_TRUE(Luminumbra::Test::PublishIndexedSave(directory, [] { return true; }));
        }
        P::RegionDigestInputs optional{.live = {},
                                       .parked = {},
                                       .durable = {},
                                       .require_index = indexed,
                                       .ground_object_bytes = {},
                                       .field_page_bytes = {}};
        auto required = optional;
        required.durable = {{0, 0, 0}};
        P::RegionDigestOperation operation(directory, indexed);
        const auto prepared = operation.prepare({0, 0}, required);
        ASSERT_EQ(prepared.chunks.size(), 1u);
        // Compatibility callers may supply a multi-region inventory. Another region's durable
        // coordinates must not require a file for an empty region or one with only live owners.
        const auto live = std::make_shared<Chunk>(IVec3(-32, 0, 0));
        live->heightmap_data = {3.0f};
        live->mark_voxel_data_dirty();
        live->set_state(ChunkState::Idle);
        auto mixed = required;
        EXPECT_TRUE(operation.prepare({-1, 0}, mixed).chunks.empty());
        mixed.live = {live};
        const auto live_only = operation.prepare({-1, 0}, mixed);
        ASSERT_EQ(live_only.chunks.size(), 1u);
        EXPECT_EQ(live_only.chunks.front(), live);
        // Three portable failures exercise both cold reads and previously cached authority.
        for (const int damage : {0, 1, 2}) {
            ASSERT_NO_FATAL_FAILURE(
                CheckRegionalReadDamage(directory, damage, operation, optional, prepared));
        }
    }
}

TEST_F(WorldOpenRefusal, SessionCaptureRefusesIncompleteParkedOwnerAtEveryActivationPhase) {
    for (const std::uint64_t activation_phase : {0u, 1u, 2u, 3u}) {
        Server::ServerWorldRunner runner(SnapshotRunnerConfig(RootString(), true));
        ASSERT_NO_FATAL_FAILURE(BootSnapshotRunner(runner, true));
        auto& session = *runner.Session();
        auto& world = *session.GetWorldSystem();
        const auto interval = static_cast<std::uint64_t>(
            world.get_last_streaming_budget_stats().update_interval_frames);
        ASSERT_EQ(interval, 4u);
        const auto phase = world.get_streaming_telemetry_stats().frames_observed % interval;
        const auto padding = (activation_phase + interval - phase) % interval;
        // At most three anchorless updates select a phase without dispatching work.
        for (std::uint64_t frame = 0; frame < padding; ++frame) {
            world.update(session.GetRegistry(), std::vector<Vec3>{}, nullptr, false);
        }
        auto chunk = std::make_shared<Chunk>(IVec3(0, 0, 0));
        chunk->mark_voxel_data_dirty();
        chunk->set_state(ChunkState::Idle);
        ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
        const auto remaining = interval - activation_phase;
        // The next activation is at most four updates away; do not mesh this owner.
        for (std::uint64_t frame = 0; frame < remaining; ++frame) {
            chunk->set_state(frame + 1 == remaining ? ChunkState::Idle : ChunkState::Loading);
            world.update(session.GetRegistry(), Vec3(32008.0f, 8.0f, 32008.0f), nullptr, false);
            world.wait_for_streaming_jobs();
        }
        ASSERT_FALSE(world.find_streamed_chunk(chunk->get_coords()));
        ASSERT_EQ(world.snapshot_parked_chunks().size(), 1u);
        chunk->set_state(ChunkState::Loading);
        const auto refused = session.CaptureAuthorityHashes();
        EXPECT_EQ(refused.status, Persistence::AuthorityCaptureStatus::IncompleteOwner);
        EXPECT_FALSE(refused.capture.has_value());
        EXPECT_TRUE(session.ComputeChunkStateHash().empty());
        EXPECT_EQ(world.find_authoritative_chunk(chunk->get_coords()), chunk);
        EXPECT_EQ(chunk->get_state(), ChunkState::Loading);
        EXPECT_EQ(session
                      .CaptureAuthorityHashes(
                          {}, Persistence::AuthorityCaptureParts::StreamedAndSubHashes)
                      .status,
                  Persistence::AuthorityCaptureStatus::Ready);
        chunk->set_state(ChunkState::Idle);
        EXPECT_EQ(session.CaptureAuthorityHashes().status,
                  Persistence::AuthorityCaptureStatus::Ready);
    }
}

TEST_F(WorldOpenRefusal, SessionCaptureRefusesMissingIncompleteAndInvalidInputs) {
    namespace P = Persistence;
    GameSession absent;
    EXPECT_EQ(absent.CaptureAuthorityHashes().status, P::AuthorityCaptureStatus::MissingCursor);
    for (const bool regions : {false, true}) {
        Server::ServerWorldRunner runner(SnapshotRunnerConfig(RootString(), regions));
        ASSERT_NO_FATAL_FAILURE(BootSnapshotRunner(runner, regions));
        auto& session = *runner.Session();
        auto chunk = std::make_shared<Chunk>(IVec3(0, 0, 0));
        chunk->mark_voxel_data_dirty();
        chunk->set_state(ChunkState::Idle);
        ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
        chunk->set_state(ChunkState::Loading);
        for (const auto parts : {P::AuthorityCaptureParts::All,
                                 P::AuthorityCaptureParts::Chunk,
                                 P::AuthorityCaptureParts::Composite,
                                 P::AuthorityCaptureParts::SubHashes,
                                 P::AuthorityCaptureParts::StreamedAndSubHashes}) {
            const auto result = session.CaptureAuthorityHashes({}, parts);
            EXPECT_EQ(result.status, P::AuthorityCaptureStatus::IncompleteOwner);
            EXPECT_FALSE(result.capture.has_value());
        }
        EXPECT_TRUE(session.ComputeChunkStateHash().empty());
        chunk->set_state(ChunkState::Idle);
        EXPECT_EQ(session.CaptureAuthorityHashes().status, P::AuthorityCaptureStatus::Ready);
        const auto invalid =
            session.CaptureAuthorityHashes({}, static_cast<P::AuthorityCaptureParts>(-1));
        EXPECT_EQ(invalid.status, P::AuthorityCaptureStatus::PreparationRefused);
        EXPECT_FALSE(invalid.capture.has_value());
    }
}

TEST_F(WorldOpenRefusal, LockstepRefusesBootAndTickCapturesBeforePublishingHashes) {
    for (const std::uint64_t refuse_tick : {std::uint64_t{0}, std::uint64_t{1}}) {
        struct CaptureFault {
            std::uint64_t tick;
            std::size_t calls = 0;
            std::vector<std::uint64_t> sink_ticks;
            std::vector<std::vector<std::uint8_t>> sink_inputs;
            std::size_t sink_hashes = 0;
        } fault{refuse_tick, 0, {}, {}, 0};
        Net::LockstepConfig config;
        config.hash_cadence_ticks = 1;
        config.system_config_identity = "config";
        config.preset_content_hash = "content";
        auto remote = config;
        remote.local_client_id = 1;
        remote.peer_client_id = 0;
        auto [local, peer] = Net::MakeLoopbackPair();
        Net::LockstepHooks hooks;
        hooks.user = &fault;
        hooks.capture_hashes = [](std::uint64_t tick, Net::HashMsg& out, void* user) {
            auto& capture_fault = *static_cast<CaptureFault*>(user);
            ++capture_fault.calls;
            out.world_hash = "partial";
            if (tick == capture_fault.tick) {
                throw Persistence::AuthorityCaptureRefused();
            }
            out.tick = tick;
            out.world_hash = "complete";
        };
        hooks.collect_local_input = [](std::uint64_t, void*) {
            return std::vector<std::uint8_t>{0x5A};
        };
        // A caller-owned replay sink sees every executed tick, including one whose
        // capture is refused, without a hash.
        hooks.record_replay_tick = [](std::uint64_t tick,
                                      const std::vector<std::uint8_t>& merged,
                                      const Net::HashMsg* hash,
                                      void* user) {
            auto& capture_fault = *static_cast<CaptureFault*>(user);
            capture_fault.sink_ticks.push_back(tick);
            capture_fault.sink_inputs.push_back(merged);
            capture_fault.sink_hashes += hash != nullptr;
        };
        Net::LockstepSession session(config, local.get(), hooks);
        const auto replay = root / ("capture-refusal-" + std::to_string(refuse_tick) + ".lrec1");
        session.SetDumpPath(replay.string());
        ASSERT_TRUE(peer->SendFrame(Net::EncodeHello(Net::MakeLockstepHello(remote))));
        if (refuse_tick == 0) {
            EXPECT_FALSE(session.Handshake());
            EXPECT_FALSE(fs::exists(replay));
            EXPECT_EQ(fault.calls, 1u);
        } else {
            ASSERT_TRUE(session.Handshake());
            ASSERT_TRUE(peer->SendFrame(Net::EncodeInput({1, 1, {0xA5}})));
            const auto result = session.PumpTick(2);
            EXPECT_EQ(result.outcome, Net::TickOutcome::Desync);
            EXPECT_FALSE(result.ran_hash_exchange);
            EXPECT_EQ(fault.calls, 2u);
            const auto recording = Replay::ReadReplay(replay.string());
            ASSERT_TRUE(recording.has_value());
            if (!recording) {
                return;
            }
            EXPECT_FALSE(recording->truncated);
            EXPECT_TRUE(recording->trailer_present);
            EXPECT_EQ(recording->tick_count, 1u);
            ASSERT_EQ(recording->inputs.size(), 1u);
            EXPECT_EQ(recording->inputs.front().tick, 1u);
            EXPECT_EQ(recording->inputs.front().inputs, (std::vector<std::uint8_t>{0x5A, 0xA5}));
            EXPECT_TRUE(recording->checkpoints.empty());
            EXPECT_EQ(fault.sink_ticks, std::vector<std::uint64_t>{1u});
            ASSERT_EQ(fault.sink_inputs.size(), 1u);
            EXPECT_EQ(fault.sink_inputs.front(), recording->inputs.front().inputs);
            EXPECT_EQ(fault.sink_hashes, 0u);
        }
        if (refuse_tick == 0) {
            EXPECT_TRUE(fault.sink_ticks.empty());
        }
        EXPECT_TRUE(session.Status().desynced);
        EXPECT_EQ(session.Status().desync_section, "authority_capture");
        EXPECT_EQ(session.AdmissionStatus().local_hashes, 0u);
        // At most one Hello plus the configured initial horizon's input frames.
        std::vector<std::uint8_t> frame;
        for (std::uint32_t i = 0; i <= config.horizon_max + 1; ++i) {
            if (!peer->TryReceiveFrame(frame)) {
                break;
            }
            Net::MessageType type{};
            ASSERT_TRUE(Net::PeekMessageType(frame, type));
            EXPECT_NE(type, Net::MessageType::Hash);
        }
        EXPECT_FALSE(peer->TryReceiveFrame(frame));
    }
}

TEST_F(WorldOpenRefusal, ServerWaterHashReadsLeaveOutsideTickChangesPending) {
    Server::ServerWorldRunner runner(SnapshotRunnerConfig(RootString(), false));
    ASSERT_NO_FATAL_FAILURE(BootSnapshotRunner(runner, false));
    auto* session = runner.Session();
    const auto chunk = PrepareServerWaterChange(*session);
    chunk->mark_water_state_dirty();
    const auto inputs = session->GetWorldSystem()->water_authority_input_items();
    const auto before = ReadServerWaterHashes(runner);
    ExpectSameServerWaterHashes(ReadServerWaterHashes(runner), before);
    EXPECT_FALSE(chunk->has_water_authority());
    EXPECT_TRUE(chunk->is_water_state_dirty());
    EXPECT_EQ(session->GetWorldSystem()->water_authority_input_items(), inputs);
}

TEST_F(WorldOpenRefusal, ServerBootWaterHashesRemainStableThroughSaveWithoutTicking) {
    Server::ServerWorldRunner runner(SnapshotRunnerConfig(RootString(), false));
    ASSERT_TRUE(runner.Boot()) << runner.GetBootError();
    const auto inputs = runner.Session()->GetWorldSystem()->water_authority_input_items();
    const auto before = ReadServerWaterHashes(runner);
    ASSERT_GT(runner.SaveFullSnapshot(), 0u);
    ExpectSameServerWaterHashes(ReadServerWaterHashes(runner), before);
    EXPECT_EQ(runner.Session()->GetWorldSystem()->water_authority_input_items(), inputs);
}

void ExpectSnapshotReloaded(const PendingSnapshotState& pending,
                            const Chunk& chunk,
                            const Server::ServerWorldRunnerConfig& config,
                            const std::string& world_id,
                            JobSystem& jobs) {
    SCOPED_TRACE(world_id);
    GameSession loaded;
    loaded.SetRootPath(config.root_path);
    loaded.SetJobSystem(&jobs);
    ASSERT_TRUE(config.systems_config_override.has_value());
    if (!config.systems_config_override.has_value())
        return;
    loaded.ApplySystemConfig(*config.systems_config_override);
    ASSERT_TRUE(loaded.LoadWorld(world_id)) << loaded.GetWorldOpenError();
    EXPECT_EQ(loaded.ComputePlantSubHash(), pending.PlantHash());
    ASSERT_NE(loaded.GetEnergyFieldState(), nullptr);
    EXPECT_EQ(loaded.GetEnergyFieldState()->CanonicalBytes(), pending.EnergyBytes());
    const auto restored_chunks = loaded.GetWorldSystem()->snapshot_streamed_chunks();
    ASSERT_EQ(restored_chunks.size(), 1u);
    EXPECT_EQ(restored_chunks.front()->get_id(), chunk.get_id());
    EXPECT_EQ(restored_chunks.front()->heightmap_data, chunk.heightmap_data);
}

class RuntimeConfigStartup : public WorldOpenRefusal {};

TEST_P(RuntimeConfigStartup, HostAndNetworkAdmissionIgnoreConflictingLaunchDefaults) {
    const auto [runtime_kind, explicit_override] = GetParam();
    const auto launch = root / "launch";
    Write(launch / "data/common/systems.json",
          R"({"sim":{"water_high_res":{"enabled":false},"active_regions":{"enabled":false}}})");
    const auto runtime_file = root / "data/common/systems.json";
    if (runtime_kind == 0)
        Write(runtime_file, R"({"sim":{"erosion":{"enabled":false}}})");
    else if (runtime_kind == 2)
        Write(runtime_file, "{\"sim\":");
    else if (runtime_kind == 3)
        fs::create_directories(runtime_file);
    class RestoreCwd {
    public:
        RestoreCwd() = default;
        ~RestoreCwd() {
            std::error_code error;
            fs::current_path(previous, error);
        }

    private:
        const fs::path previous = fs::current_path();
    };
    const RestoreCwd restore_cwd;
    fs::current_path(launch);

    ServerCliOptions options;
    options.root = RootString();
    options.surface_radius = 0;
    options.collision_radius = 0;
    options.test_streaming_radius_cap = 1;
    options.autosave_ticks = 0;
    if (explicit_override != 0) {
        options.systems_config_override = luminumbra::core::SystemConfig::FromJsonString(
            R"({"sim":{"plant_growth":{"enabled":false}}})");
    }
    const auto expected =
        options.systems_config_override
            ? *options.systems_config_override
            : luminumbra::core::SystemConfig::FromJsonString(
                  runtime_kind == 0 ? R"({"sim":{"erosion":{"enabled":false}}})" : "{}");
    GameSession expected_session;
    expected_session.ApplySystemConfig(expected);
    const auto identity = expected_session.SystemConfigIdentity();
    const auto hello = RuntimeHelloFrom(options);
    EXPECT_EQ(hello.system_config_identity, identity);
    ASSERT_FALSE(hello.preset_content_hash.empty());
    Luminumbra::Replay::ReplayHeader replay_header;
    replay_header.seed_string = options.seed;
    replay_header.preset = options.preset;
    replay_header.surface_radius = 0;
    replay_header.collision_radius = 0;
    replay_header.system_config_identity = identity;
    replay_header.preset_content_hash = hello.preset_content_hash;
    const auto replay_config = ReplayRunnerConfigFrom(options, replay_header);
    ASSERT_TRUE(replay_config.has_value());
    if (!replay_config.has_value())
        return;
    ASSERT_TRUE(replay_config->systems_config_override.has_value());
    if (!replay_config->systems_config_override.has_value())
        return;
    GameSession replay_identity;
    replay_identity.ApplySystemConfig(*replay_config->systems_config_override);
    EXPECT_EQ(replay_identity.SystemConfigIdentity(), identity);
    auto mismatched_header = replay_header;
    mismatched_header.system_config_identity = "invalid";
    EXPECT_FALSE(ReplayRunnerConfigFrom(options, mismatched_header).has_value());
    Server::ServerWorldRunner runner(RunnerConfigFrom(options));
    ASSERT_TRUE(runner.Boot()) << runner.GetBootError();
    ASSERT_NE(runner.Session(), nullptr);
    EXPECT_EQ(runner.Session()->SystemConfigIdentity(), identity);
    runner.Shutdown();
}

INSTANTIATE_TEST_SUITE_P(RuntimeDefaultsAndOverrides,
                         RuntimeConfigStartup,
                         testing::Combine(testing::Values(0, 1, 2, 3), testing::Values(0, 1)));

class SnapshotEnergyFailure : public WorldOpenRefusal {};

TEST_P(SnapshotEnergyFailure, FailureKeepsDirtyChunksAndRetryPersistsAllState) {
    const auto [regions, snapshot_path] = GetParam();
    const bool full_snapshot = snapshot_path >= 3;
    const int snapshot_mode = snapshot_path % 3;
    const bool active_regions = regions != 0;
    const bool dirty = snapshot_mode != 2;
    const auto config = SnapshotRunnerConfig(RootString(), active_regions);
    Server::ServerWorldRunner runner(config);
    ASSERT_NO_FATAL_FAILURE(BootSnapshotRunner(runner, active_regions));
    auto* session = runner.Session();
    const auto destination = session->GetWorldSaveDir();
    std::shared_ptr<Chunk> chunk;
    ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(*session, chunk, snapshot_mode != 0));
    if (!dirty) {
        ASSERT_TRUE(session->SaveWorldState());
        ASSERT_FALSE(chunk->is_voxel_data_dirty());
        auto* energy = session->GetEnergyFieldState();
        energy->QueueDeposit(3, 0, 0, 0, 500);
        ASSERT_NO_FATAL_FAILURE(ApplySnapshotTestEnergy(*session, 0));
    }
    // Retain the prior energy file while injecting the energy-only failure.
    const PendingSnapshotState pending(*session);
    const auto metadata_bytes = Read(destination / "world_info.json");
    const auto energy_file = destination / "aether_state.efs";
    const auto preserved_energy = root / "preserved-energy.efs";
    const bool had_energy = fs::exists(energy_file);
    const auto old_energy_bytes = had_energy ? Read(energy_file) : std::string{};
    if (had_energy)
        fs::rename(energy_file, preserved_energy);
    ASSERT_TRUE(fs::create_directory(energy_file));
    Write(energy_file / "keep", "destination occupied");

    world::WorldStateSaveReport report;
    report.saved = true;
    report.chunks_saved = 99;
    if (full_snapshot) {
        EXPECT_EQ(runner.SaveFullSnapshot(), 0u);
        // The full writer returns a chunk count; inspect the same pending state below.
    } else {
        EXPECT_FALSE(session->SaveWorldState(&report));
        EXPECT_FALSE(report.saved);
        EXPECT_EQ(report.chunks_saved, 0u);
        EXPECT_EQ(report.chunks_dirty, dirty ? 1u : 0u);
    }
    EXPECT_EQ(chunk->is_voxel_data_dirty(), dirty);
    EXPECT_EQ(Read(destination / "world_info.json"), metadata_bytes);
    EXPECT_EQ(Read(energy_file / "keep"), "destination occupied");
    EXPECT_EQ(session->GetEnergyFieldState()->CanonicalBytes(), pending.EnergyBytes());
    EXPECT_EQ(session->ComputePlantSubHash(), pending.PlantHash());
    if (dirty)
        pending.ExpectUnchanged(*session, *chunk);
    if (had_energy) {
        EXPECT_EQ(Read(preserved_energy), old_energy_bytes);
    }

    fs::remove_all(energy_file);
    if (had_energy)
        fs::rename(preserved_energy, energy_file);
    if (full_snapshot) {
        ASSERT_EQ(runner.SaveFullSnapshot(), 1u);
    } else {
        ASSERT_TRUE(session->SaveWorldState(&report));
        EXPECT_EQ(report.chunks_dirty, dirty ? 1u : 0u);
        if (dirty) {
            EXPECT_TRUE(report.saved);
            EXPECT_EQ(report.chunks_saved, 1u);
        }
    }
    EXPECT_FALSE(chunk->is_voxel_data_dirty());
    ASSERT_NO_FATAL_FAILURE(
        ExpectSnapshotReloaded(pending, *chunk, config, session->GetMetadata().worldId, jobs));
    runner.Shutdown();
}

// One bounded case per region mode, writer, and first/incremental/clean snapshot path.
INSTANTIATE_TEST_SUITE_P(RegionModesAndSnapshotPaths,
                         SnapshotEnergyFailure,
                         testing::Combine(testing::Values(0, 1), testing::Range(0, 6)));

// Refuse the selected sibling publication after chunk and manifest progress.
// Match the actual destination on every platform, independently of how many
// directory entries the durable writer flushes before reaching it.
class SiblingWriteFailure {
public:
    SiblingWriteFailure() = default;
    ~SiblingWriteFailure() {
        Reset();
    }
    SiblingWriteFailure(const SiblingWriteFailure&) = delete;
    SiblingWriteFailure& operator=(const SiblingWriteFailure&) = delete;

    void Arm(const fs::path& destination, bool energy) {
        Target() = energy ? destination / "aether_state.efs"
                          : WorldSaveService::plant_entities_path(destination);
        Reached() = false;
        Persistence::detail::set_file_publication_hook_for_testing(&Publish);
    }

    void ArmMetadata(const fs::path& destination) {
        Target() = destination / "world_info.json";
        Reached() = false;
        Persistence::detail::set_file_publication_hook_for_testing(&Publish);
    }

    void ArmPlantRemoval(const fs::path& destination, const fs::path& displaced) {
        Target() = WorldSaveService::plant_entities_path(destination);
        Displaced() = displaced;
        Reached() = false;
        Persistence::detail::set_file_publication_hook_for_testing(&SubstitutePlant);
    }

    void Reset() {
        Persistence::detail::set_file_publication_hook_for_testing(nullptr);
        // A writer may already have loaded the hook. Keep its target immutable
        // until this session has drained, before the next fixture arms it.
    }

    bool reached() const {
        return Reached();
    }

private:
    static fs::path& Displaced() {
        static fs::path displaced;
        return displaced;
    }
    static fs::path& Target() {
        static fs::path target;
        return target;
    }
    static bool& Reached() {
        static bool reached = false;
        return reached;
    }
    static bool Publish(const fs::path& path, bool removing) {
        if (!removing && path == Target()) {
            Reached() = true;
            return false;
        }
        return true;
    }
    static bool SubstitutePlant(const fs::path& path, bool removing) {
        if (removing && path == Target() && !Reached()) {
            fs::rename(path, Displaced());
            Write(path, "foreign plant occupant");
            Reached() = true;
        }
        return true;
    }
};

class CompleteSaveFailure : public WorldOpenRefusal {};

namespace {
struct SessionSaveOverlap {
    std::barrier<> staged{2};
    std::barrier<> release{2};
    std::atomic<bool> hold_next{true};
};

SessionSaveOverlap*& ActiveSessionSaveOverlap() {
    static SessionSaveOverlap* overlap = nullptr;
    return overlap;
}

bool HoldSessionMetadata() {
    if (!ActiveSessionSaveOverlap()->hold_next.exchange(false))
        return true;
    ActiveSessionSaveOverlap()->staged.arrive_and_wait();
    ActiveSessionSaveOverlap()->release.arrive_and_wait();
    return true;
}
TEST_F(WorldOpenRefusal, OwnerContinuationDescriptorsRefuseBeforeEveryAdoptionPath) {
    GameSession source_session;
    source_session.SetJobSystem(&jobs);
    source_session.SetRootPath(RootString());
    source_session.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(source_session.CreateTransientWorld("Owner source", "1337", "default"));
    auto& source = *source_session.GetPhysicsSystem();
    const auto index = source.create_avatar_character(Vec3(8.25f, 40.125f, 8.75f));
    source.set_avatar_wish_velocity(index, glm::vec2(0.125f, 0.25f));
    source.update_avatars(1.0f / 30.0f);
    Persistence::OwnerContinuation record;
    record.world_seed = 1337;
    record.origin = Vec3(8, 18, 8);
    record.anchor = source.get_avatar_position(index);
    World::PlayerAvatarOwnerState avatar;
    avatar.avatar.position = record.anchor;
    avatar.avatar.velocity = source.get_avatar_velocity(index);
    ASSERT_TRUE(source_session.CaptureAvatarOwner(avatar.avatar, index, avatar));
    record.avatars.push_back(avatar);
    const auto sphere = source.create_dynamic_sphere(Vec3(30, 50, 30), Vec3(0.1f, 0, 0.2f), 0.3f);
    ASSERT_FALSE(sphere.IsInvalid());
    Persistence::PhysicsOwnerContinuation body;
    body.owner_id = sphere.GetIndexAndSequenceNumber();
    body.anchor = source.get_body_position(sphere);
    ASSERT_TRUE(source.capture_body_owner(sphere, body.state));
    record.bodies.push_back(body);
    source.create_player_controller(Vec3(40, 50, 40));
    record.player.emplace();
    record.player->anchor = source.get_player_position();
    ASSERT_TRUE(source_session.CapturePlayerOwner(record.player->state));
    Systems::PhysicsSystem::BodySetState allocator;
    ASSERT_TRUE(source.capture_body_owners(allocator));
    record.slot_count = allocator.slot_count;
    record.free_slots = allocator.free_slots;
    GameSession live({.owner_continuation = true});
    live.SetRootPath(RootString());
    live.SetJobSystem(&jobs);
    ASSERT_TRUE(live.LoadWorld("fixture"));
    record.preset_identity = live.CaptureWorldPreparationOptions().owner_preset_identity;
    std::string encoded;
    ASSERT_TRUE(Persistence::EncodeOwnerContinuation(record, encoded));
    const auto valid = nlohmann::json::parse(encoded);
    const auto original_metadata = nlohmann::json::parse(Read(save / "world_info.json"));
    auto admitted_metadata = original_metadata;
    admitted_metadata["owner_continuation"] = encoded;
    Write(save / "world_info.json", admitted_metadata.dump());
    {
        GameSession admitted({.owner_continuation = true});
        admitted.SetRootPath(RootString());
        admitted.SetJobSystem(&jobs);
        ASSERT_TRUE(admitted.LoadWorld("fixture")) << admitted.GetWorldOpenError();
        ASSERT_TRUE(admitted.LoadWorldStateFrom(save)) << admitted.GetWorldOpenError();
        const auto prepared =
            GameSession::PrepareWorldLoad(admitted.CaptureWorldPreparationOptions(), "fixture");
        ASSERT_TRUE(admitted.AdoptPreparedWorld(prepared)) << admitted.GetWorldOpenError();
    }
    Write(save / "world_info.json", original_metadata.dump());
    const auto* world = live.GetWorldSystem();
    const auto* physics = live.GetPhysicsSystem();
    const auto revision = live.WorldRevision();
    const auto id = live.GetMetadata().worldId;
    const auto chunk_hash = live.ComputeChunkStateHash();
    const auto sentinel = live.GetRegistry().create();
    std::vector<std::pair<std::string, nlohmann::json>> invalid;
    const auto add = [&](const std::string& name, const nlohmann::json& value) {
        invalid.emplace_back(name, value.dump());
    };
    auto changed = valid;
    changed["world_seed"] = 1338u;
    add("foreign world seed", changed);
    changed = valid;
    changed["preset_identity"] = std::string(16, '0');
    add("foreign preset", changed);
    changed = valid;
    changed["version"] = 3u;
    add("version", changed);
    invalid.emplace_back("truncated", encoded.substr(0, encoded.size() / 2));
    invalid.emplace_back("empty", "");
    invalid.emplace_back("non-string record", valid);
    invalid.emplace_back("null record", nullptr);
    invalid.emplace_back("oversized",
                         std::string(Persistence::kOwnerContinuationRecordBytes + 1, ' '));
    changed = valid;
    changed["origin"][0] = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::infinity());
    add("non-finite origin", changed);
    changed = valid;
    changed["origin"][1] = std::bit_cast<std::uint32_t>(1.0e38f);
    add("finite origin outside chunk range", changed);
    changed = valid;
    changed["anchor"][0] = std::bit_cast<std::uint32_t>(16777216.0f);
    add("finite anchor outside chunk range", changed);
    changed = valid;
    changed["anchor"][0] = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN());
    add("non-finite moving anchor", changed);
    changed = valid;
    changed["avatars"][0]["anchor"][0] = 0u;
    add("avatar anchor disagreement", changed);
    changed = valid;
    changed["avatars"].push_back(changed["avatars"][0]);
    changed["avatar_count"] = 2u;
    add("duplicate id", changed);
    changed = valid;
    changed["avatars"][0]["id"] = 99u;
    add("unknown id", changed);
    changed = valid;
    changed["avatar_count"] = 0u;
    add("roster disagreement", changed);
    changed = valid;
    changed["avatar_count"] = 4097u;
    add("avatar count bound", changed);
    changed = valid;
    changed["body_count"] = 129u;
    add("body count bound", changed);
    changed = valid;
    changed["player"]["id"] = 1u;
    add("unknown player owner", changed);
    changed = valid;
    changed["player"]["anchor"][0] =
        std::bit_cast<std::uint32_t>(std::numeric_limits<float>::infinity());
    add("non-finite player anchor", changed);
    changed = valid;
    changed["player"]["anchor"][0] = 0u;
    add("player anchor disagreement", changed);
    changed = valid;
    changed["bodies"][0]["anchor"][0] = 0u;
    add("body anchor disagreement", changed);
    changed = valid;
    changed["bodies"][0]["id"] = 100u;
    add("body id disagreement", changed);
    changed = valid;
    changed["bodies"].push_back(changed["bodies"][0]);
    changed["body_count"] = 2u;
    add("duplicate body", changed);
    changed = valid;
    changed["bodies"][0]["state"] = "00";
    add("truncated physics state", changed);
    changed = valid;
    changed["avatars"][0]["state"] = std::string(2 * (World::kMaxAvatarOwnerBytes + 1), '0');
    add("oversized avatar payload", changed);
    changed = valid;
    changed["avatar_count"] = -1;
    add("negative roster count", changed);
    changed = valid;
    changed["stream_tick"] = world::WorldClock::kTickLimit + 1;
    add("stream clock bound", changed);
    changed = valid;
    changed["simulation_tick"] = 1u;
    add("clock disagreement", changed);
    changed = valid;
    changed["moving_anchor"] = true;
    add("moving mode disagreement", changed);
    changed = valid;
    changed["water_smoke"] = true;
    add("water mode disagreement", changed);
    changed = valid;
    changed["avatars"][0]["state"] = "00";
    add("truncated C2 owner", changed);
    changed = valid;
    changed["avatars"][0]["state"] = "xx";
    add("invalid C2 encoding", changed);
    changed = valid;
    changed["avatars"][0]["state"] = "!!!!";
    add("invalid base64 alphabet", changed);
    changed = valid;
    changed["avatars"][0]["state"] = "AB==";
    add("noncanonical base64 padding", changed);
    changed = valid;
    changed.erase("avatar_count");
    add("missing descriptor field", changed);
    changed = valid;
    changed["avatars"] = nullptr;
    add("wrong roster type", changed);
    changed = valid;
    changed["anchor"] = {0u, 0u};
    add("partial anchor", changed);
    changed = valid;
    changed["terrain"] = {{{"chunk_id", 0u}, {"body_id", 10240u}}};
    add("unknown terrain id", changed);
    changed = valid;
    changed["terrain"] = {{{"chunk_id", UINT64_MAX}, {"body_id", 2u}}};
    add("missing terrain owner", changed);
    changed = valid;
    changed["terrain"] = {{{"chunk_id", 0u}, {"body_id", 0u}}, {{"chunk_id", 1u}, {"body_id", 0u}}};
    add("conflicting terrain slots", changed);
    changed = valid;
    changed["terrain"] = {{{"chunk_id", 1u}, {"body_id", 3u}}, {{"chunk_id", 0u}, {"body_id", 4u}}};
    add("unsorted terrain", changed);
    changed = valid;
    changed["slot_count"] = 10241u;
    add("allocator extent bound", changed);
    changed = valid;
    changed["free_slots"] = {0u};
    add("occupied free slot", changed);
    changed = valid;
    changed["free_slots"] = {UINT64_MAX};
    add("free id width", changed);
    changed = valid;
    changed["simulation_bodies"] = {0u};
    add("conflicting simulation terrain", changed);
    changed = valid;
    changed["simulation_columns"] = {{{"id", 0u}, {"heights", nlohmann::json::array()}}};
    add("partial simulation terrain", changed);
    changed = valid;
    changed["simulation_bodies"] = std::vector<std::uint32_t>(129, 0u);
    add("simulation terrain bound", changed);
    const auto bad_terrain = [&](std::size_t sample_count, std::uint64_t first) {
        auto value = valid;
        value["slot_count"] = 2u;
        value["simulation_bodies"] = {1u};
        auto heights = std::vector<std::uint64_t>(sample_count, 0u);
        if (!heights.empty()) {
            heights[0] = first;
        }
        value["simulation_columns"] = {{{"id", 0u}, {"heights", heights}}};
        return value;
    };
    constexpr std::size_t samples = (CHUNK_SIZE_X + 1u) * (CHUNK_SIZE_Z + 1u);
    add("partial terrain samples", bad_terrain(samples - 1, 0u));
    add("terrain sample width", bad_terrain(samples, UINT64_MAX));
    add("nonfinite terrain sample",
        bad_terrain(samples, std::bit_cast<std::uint32_t>(std::numeric_limits<float>::infinity())));
    add("terrain sample outside coordinate range",
        bad_terrain(samples, std::bit_cast<std::uint32_t>(1.0e38f)));
    changed = bad_terrain(samples, 0u);
    changed["simulation_columns"][0]["id"] = UINT64_MAX;
    add("unknown simulation terrain column", changed);
    changed = valid;
    changed["bodies"][0]["suspended"] = "yes";
    add("invalid suspension flag", changed);
    changed = valid;
    changed["bodies"][0]["velocity"] = {0u};
    add("partial retained velocity", changed);
    changed = valid;
    changed["bodies"][0]["velocity"] = {UINT64_MAX, 0u, 0u};
    add("retained velocity width", changed);
    changed = valid;
    changed["bodies"][0]["velocity"] = {
        std::bit_cast<std::uint32_t>(std::numeric_limits<float>::infinity()), 0u, 0u};
    add("nonfinite retained velocity", changed);
    changed = valid;
    changed["bodies"][0]["registration_slot"] = 10241u;
    add("registration bound", changed);
    for (const auto& [name, bytes] : invalid) {
        SCOPED_TRACE(name);
        auto metadata = original_metadata;
        metadata["owner_continuation"] = bytes;
        Write(save / "world_info.json", metadata.dump());
        const auto disk = DiskBytes(root);
        EXPECT_FALSE(live.LoadWorld("fixture"));
        EXPECT_FALSE(live.LoadWorldStateFrom(save));
        const auto prepared =
            GameSession::PrepareWorldLoad(live.CaptureWorldPreparationOptions(), "fixture");
        EXPECT_FALSE(live.AdoptPreparedWorld(prepared));
        EXPECT_EQ(live.GetWorldSystem(), world);
        EXPECT_EQ(live.GetPhysicsSystem(), physics);
        EXPECT_EQ(live.WorldRevision(), revision);
        EXPECT_EQ(live.GetMetadata().worldId, id);
        EXPECT_EQ(live.ComputeChunkStateHash(), chunk_hash);
        EXPECT_TRUE(live.GetRegistry().valid(sentinel));
        EXPECT_EQ(DiskBytes(root), disk);
    }
    Write(save / "world_info.json", admitted_metadata.dump());
    const auto dependency = root / "data/common/biomes.json";
    const auto dependency_bytes = Read(dependency);
    Write(dependency, dependency_bytes + "\n");
    const auto changed_preset =
        world::LoadTerrainPreset(root / "worlds/atlas/presets-rev6/default.json", root / "data");
    ASSERT_TRUE(changed_preset.ok);
    ASSERT_NE(changed_preset.preset_content_hash, live.PresetContentHash());
    const auto changed_disk = DiskBytes(root);
    EXPECT_FALSE(live.LoadWorld("fixture"));
    const auto changed_preparation =
        GameSession::PrepareWorldLoad(live.CaptureWorldPreparationOptions(), "fixture");
    EXPECT_FALSE(live.AdoptPreparedWorld(changed_preparation));
    EXPECT_EQ(live.GetWorldSystem(), world);
    EXPECT_EQ(live.GetPhysicsSystem(), physics);
    EXPECT_EQ(live.WorldRevision(), revision);
    EXPECT_EQ(live.ComputeChunkStateHash(), chunk_hash);
    EXPECT_TRUE(live.GetRegistry().valid(sentinel));
    EXPECT_EQ(DiskBytes(root), changed_disk);
    Write(dependency, dependency_bytes);
    Write(save / "world_info.json", original_metadata.dump());
    EXPECT_TRUE(live.SaveWorldState());
}

TEST_F(WorldOpenRefusal, OwnerContinuationRunnerRestoresRosterAndRejectsConflictingBoot) {
    for (const bool regions : {false, true}) {
        SCOPED_TRACE(regions);
        auto config = SnapshotRunnerConfig(RootString(), regions);
        config.avatar_count = 1;
        Server::ServerWorldRunner original(config, {.owner_continuation = true});
        ASSERT_TRUE(original.Boot()) << original.GetBootError();
        EXPECT_EQ(original.SpawnedAvatarCount(), 1u);
        if (regions) {
            const std::vector<Vec3> oversized(Systems::PhysicsSystem::kMaxRestoredAvatars + 1);
            EXPECT_THROW(original.Session()->SetOwnerContinuationSimulationAnchors(oversized),
                         std::logic_error);
        }
        original.SetAvatarMove(0, 0.125f, 0.25f);
        ASSERT_EQ(original.RunFixedTicks(3).ticks_executed, 3u);
        ASSERT_GT(original.SaveFullSnapshot(), 0u);
        config.world_id = original.Session()->GetMetadata().worldId;
        Server::ServerWorldRunner restored(config, {.owner_continuation = true});
        ASSERT_TRUE(restored.Boot()) << restored.GetBootError();
        EXPECT_EQ(restored.SpawnedAvatarCount(), 0u);
        ASSERT_EQ(restored.Avatars().size(), 1u);
        for (int tick = 0; tick < 4; ++tick) {
            World::PlayerAvatarOwnerState expected, actual;
            ASSERT_TRUE(original.CaptureAvatarOwner(0, expected));
            ASSERT_TRUE(restored.CaptureAvatarOwner(0, actual));
            EXPECT_EQ(expected.physics.size, actual.physics.size);
            EXPECT_EQ(expected.physics.bytes, actual.physics.bytes);
            EXPECT_EQ(original.ComputeWorldSubHashes().entities,
                      restored.ComputeWorldSubHashes().entities);
            ExpectSameServerSimulationHashes(ReadServerWaterHashes(restored),
                                             ReadServerWaterHashes(original));
            ASSERT_EQ(original.RunFixedTicks(1).ticks_executed, 1u);
            ASSERT_EQ(restored.RunFixedTicks(1).ticks_executed, 1u);
            EXPECT_EQ(original.OwnerAnchorVisitCount(), original.Avatars().size());
            EXPECT_EQ(restored.OwnerAnchorVisitCount(), restored.Avatars().size());
        }
        ++config.avatar_count;
        const auto before = DiskBytes(root);
        Server::ServerWorldRunner conflict(config, {.owner_continuation = true});
        EXPECT_FALSE(conflict.Boot());
        EXPECT_EQ(conflict.SpawnedAvatarCount(), 0u);
        EXPECT_EQ(DiskBytes(root), before);
    }
}

TEST_F(WorldOpenRefusal, OwnerContinuationDefaultOffRetainsLegacyBytesAndAuthorityHashes) {
    auto config = SnapshotRunnerConfig(RootString(), false);
    Server::ServerWorldRunner source(config);
    ASSERT_TRUE(source.Boot());
    ASSERT_GT(source.SaveFullSnapshot(), 0u);
    config.world_id = source.Session()->GetMetadata().worldId;
    const auto directory = source.Session()->GetWorldSaveDir();
    const auto saved_bytes = DiskBytes(directory);
    EXPECT_FALSE(
        nlohmann::json::parse(Read(directory / "world_info.json")).contains("owner_continuation"));
    // Compare the same lifecycle on the same input. The ordinary loaded boot
    // remeshes derivatives, so fresh-boot bytes are not a legacy resave oracle.
    Server::ServerWorldRunner legacy(config);
    ASSERT_TRUE(legacy.Boot());
    Server::ServerWorldRunner disabled(config, {.owner_continuation = false});
    ASSERT_TRUE(disabled.Boot());
    EXPECT_EQ(disabled.SpawnedAvatarCount(), 0u);
    EXPECT_EQ(DiskBytes(directory), saved_bytes);
    const auto expected = ReadServerWaterHashes(legacy);
    EXPECT_EQ(expected.world, source.ComputeWorldHash());
    ExpectSameServerWaterHashes(ReadServerWaterHashes(disabled), expected);
    ASSERT_GT(legacy.SaveFullSnapshot(), 0u);
    const auto bytes = DiskBytes(directory);
    ASSERT_GT(disabled.SaveFullSnapshot(), 0u);
    const auto actual = ReadServerWaterHashes(disabled);
    EXPECT_EQ(actual.world, expected.world);
    ExpectSameServerSubHashes(actual.sub, expected.sub);
    ExpectSameServerWaterHashes(actual, ReadServerWaterHashes(legacy));
    EXPECT_EQ(DiskBytes(directory), bytes);
}

TEST_F(WorldOpenRefusal, OwnerContinuationMovingAnchorResumesAcrossRunBoundaries) {
    auto config = SnapshotRunnerConfig(RootString(), false);
    config.moving_anchor = true;
    Server::ServerWorldRunner original(config, {.owner_continuation = true});
    ASSERT_TRUE(original.Boot());
    constexpr std::uint64_t saved_ticks = 2u * (CHUNK_SIZE_X + 1u);
    ASSERT_EQ(original.RunFixedTicks(saved_ticks).ticks_executed, saved_ticks);
    EXPECT_EQ(original.Session()->GetWorldSystem()->pending_publication_chunks(), 0u);
    ASSERT_GT(original.SaveFullSnapshot(), 0u);
    config.world_id = original.Session()->GetMetadata().worldId;
    Server::ServerWorldRunner restored(config, {.owner_continuation = true});
    ASSERT_TRUE(restored.Boot()) << restored.GetBootError();
    ASSERT_NE(restored.Session()->RestoredOwnerContinuation(), nullptr);
    EXPECT_EQ(restored.Session()->RestoredOwnerContinuation()->stream_tick, saved_ticks);
    ExpectSameServerSimulationHashes(ReadServerWaterHashes(restored),
                                     ReadServerWaterHashes(original));
    std::vector<std::pair<ChunkID, JPH::BodyID>> original_terrain, restored_terrain;
    ASSERT_TRUE(
        original.Session()->GetPhysicsSystem()->capture_chunk_collision_owners(original_terrain));
    ASSERT_TRUE(
        restored.Session()->GetPhysicsSystem()->capture_chunk_collision_owners(restored_terrain));
    EXPECT_EQ(restored_terrain, original_terrain);
    ASSERT_EQ(original.RunFixedTicks(2).ticks_executed, 2u);
    ASSERT_EQ(restored.RunFixedTicks(2).ticks_executed, 2u);
    ExpectSameServerSimulationHashes(ReadServerWaterHashes(restored),
                                     ReadServerWaterHashes(original));
    EXPECT_EQ(original.OwnerAnchorVisitCount(), 0u);
    EXPECT_EQ(restored.OwnerAnchorVisitCount(), 0u);
    ASSERT_GT(original.SaveFullSnapshot(), 0u);
    const auto original_record =
        nlohmann::json::parse(Read(original.Session()->GetWorldSaveDir() / "world_info.json"))
            .at("owner_continuation");
    ASSERT_GT(restored.SaveFullSnapshot(), 0u);
    const auto restored_record =
        nlohmann::json::parse(Read(restored.Session()->GetWorldSaveDir() / "world_info.json"))
            .at("owner_continuation");
    EXPECT_EQ(original_record, restored_record);
    // Restore at two offsets of the four-update cadence. With a process-local
    // counter, at least one restore would activate at a different crossing tick,
    // regardless of the fresh boot's final update phase.
    Server::ServerWorldRunner second_restored(config, {.owner_continuation = true});
    ASSERT_TRUE(second_restored.Boot()) << second_restored.GetBootError();
    // Cross another residency boundary after restoring at a different update
    // phase. Compare each process at the same cumulative tick, including when
    // one caller batches ticks and the other returns after every tick.
    // The moving anchor travels half a metre per streaming tick.
    constexpr std::uint64_t more_ticks = 4u * CHUNK_SIZE_X - (saved_ticks + 2u);
    ASSERT_EQ(original.RunFixedTicks(more_ticks).ticks_executed, more_ticks);
    for (std::uint64_t tick = 0; tick < more_ticks; ++tick) {
        ASSERT_EQ(restored.RunFixedTicks(1).ticks_executed, 1u);
        EXPECT_EQ(restored.Session()->GetWorldSystem()->pending_publication_chunks(), 0u);
    }
    ExpectSameServerSimulationHashes(ReadServerWaterHashes(restored),
                                     ReadServerWaterHashes(original));
    ASSERT_EQ(second_restored.RunFixedTicks(more_ticks).ticks_executed, more_ticks);
    ExpectSameServerSimulationHashes(ReadServerWaterHashes(second_restored),
                                     ReadServerWaterHashes(original));
    // Allow the crossing's generated terrain to reach its collision pass too.
    ASSERT_EQ(original.RunFixedTicks(4).ticks_executed, 4u);
    ASSERT_EQ(restored.RunFixedTicks(4).ticks_executed, 4u);
    ASSERT_EQ(second_restored.RunFixedTicks(4).ticks_executed, 4u);
    ExpectSameServerSimulationHashes(ReadServerWaterHashes(restored),
                                     ReadServerWaterHashes(original));
    ExpectSameServerSimulationHashes(ReadServerWaterHashes(second_restored),
                                     ReadServerWaterHashes(original));
    original_terrain.clear();
    restored_terrain.clear();
    ASSERT_TRUE(
        original.Session()->GetPhysicsSystem()->capture_chunk_collision_owners(original_terrain));
    ASSERT_TRUE(
        restored.Session()->GetPhysicsSystem()->capture_chunk_collision_owners(restored_terrain));
    EXPECT_EQ(restored_terrain, original_terrain);
    restored_terrain.clear();
    ASSERT_TRUE(second_restored.Session()->GetPhysicsSystem()->capture_chunk_collision_owners(
        restored_terrain));
    EXPECT_EQ(restored_terrain, original_terrain);
}

TEST_F(WorldOpenRefusal, OwnerContinuationRunnerRestoresEmptySnapshotWithoutWarmup) {
    auto config = SnapshotRunnerConfig(RootString(), false);
    Server::ServerWorldRunner original(config, {.owner_continuation = true});
    ASSERT_TRUE(original.Boot());
    ASSERT_EQ(original.RunFixedTicks(1).ticks_executed, 1u);
    original.Session()->GetWorldSystem()->clear_world(original.Session()->GetPhysicsSystem());
    ASSERT_TRUE(original.Session()->SaveWorldState());
    const auto expected = original.Session()->ComputeChunkStateHash();
    config.world_id = original.Session()->GetMetadata().worldId;
    const auto bytes = DiskBytes(root);
    Server::ServerWorldRunner restored(config, {.owner_continuation = true});
    ASSERT_TRUE(restored.Boot()) << restored.GetBootError();
    ASSERT_NE(restored.Session()->RestoredOwnerContinuation(), nullptr);
    EXPECT_EQ(restored.Session()->GetLastLoadedChunkCount(), 0u);
    EXPECT_EQ(restored.Session()->GetWorldSystem()->snapshot_streamed_chunks().size(), 0u);
    EXPECT_EQ(restored.Session()->ComputeChunkStateHash(), expected);
    EXPECT_EQ(restored.Session()->GetSimulationTickCount(), 1u);
    EXPECT_EQ(restored.Session()->RestoredOwnerContinuation()->stream_tick, 1u);
    EXPECT_EQ(restored.GetBootSettleStats().iterations, 0);
    EXPECT_TRUE(restored.GetBootSettleStats().water_settle_skipped);
    EXPECT_EQ(DiskBytes(root), bytes);
}

TEST_F(WorldOpenRefusal, OwnerContinuationRunnerRefusesRosterBoundsAndExhaustedClock) {
    auto config = SnapshotRunnerConfig(RootString(), false);
    const auto disk = DiskBytes(root);
    for (const int count : {-1, 4097}) {
        SCOPED_TRACE(count);
        config.avatar_count = count;
        Server::ServerWorldRunner invalid(config, {.owner_continuation = true});
        EXPECT_FALSE(invalid.Boot());
        EXPECT_EQ(invalid.Session(), nullptr);
        EXPECT_EQ(invalid.SpawnedAvatarCount(), 0u);
        EXPECT_EQ(DiskBytes(root), disk);
    }
    config.avatar_count = 0;
    Server::ServerWorldRunner original(config, {.owner_continuation = true});
    ASSERT_TRUE(original.Boot());
    const auto spawn = original.Session()->GetMetadata().spawnPoint;
    original.Session()->SetOwnerContinuationAnchor(spawn, spawn, world::WorldClock::kTickLimit);
    ASSERT_GT(original.SaveFullSnapshot(), 0u);
    config.world_id = original.Session()->GetMetadata().worldId;
    Server::ServerWorldRunner restored(config, {.owner_continuation = true});
    ASSERT_TRUE(restored.Boot()) << restored.GetBootError();
    const auto before = DiskBytes(root);
    const auto hashes = ReadServerWaterHashes(restored);
    const auto report = restored.RunFixedTicks(1);
    EXPECT_EQ(report.ticks_executed, 0u);
    EXPECT_EQ(report.frames_executed, 0u);
    EXPECT_TRUE(report.region_fault);
    ExpectSameServerWaterHashes(ReadServerWaterHashes(restored), hashes);
    EXPECT_EQ(DiskBytes(root), before);
}

TEST_F(WorldOpenRefusal, OwnerContinuationRefusesStreamingPastTheCoordinateBound) {
    auto config = SnapshotRunnerConfig(RootString(), false);
    config.moving_anchor = true;
    Server::ServerWorldRunner runner(config, {.owner_continuation = true});
    ASSERT_TRUE(runner.Boot());
    const Vec3 edge{16777215.0f, 18, 16777215.0f};
    runner.Session()->SetOwnerContinuationAnchor(
        edge, runner.Session()->GetMetadata().spawnPoint, 0);
    ASSERT_GT(runner.SaveFullSnapshot(), 0u);
    config.world_id = runner.Session()->GetMetadata().worldId;
    // Tick zero retains the boot anchor before movement starts from its origin.
    // The first half-metre step rounds outside the coordinate range.
    Server::ServerWorldRunner restored(config, {.owner_continuation = true});
    ASSERT_TRUE(restored.Boot()) << restored.GetBootError();
    const auto before = DiskBytes(root);
    const auto report = restored.RunFixedTicks(1);
    EXPECT_TRUE(report.region_fault);
    EXPECT_EQ(report.ticks_executed, 0u);
    EXPECT_EQ(report.frames_executed, 0u);
    EXPECT_EQ(DiskBytes(root), before);
}

TEST_F(WorldOpenRefusal, OwnerContinuationMetadataFailureRollsBackAndKeepsOwnersPending) {
    auto config = SnapshotRunnerConfig(RootString(), false);
    config.avatar_count = 1;
    Server::ServerWorldRunner runner(config, {.owner_continuation = true});
    ASSERT_TRUE(runner.Boot());
    ASSERT_GT(runner.SaveFullSnapshot(), 0u);
    runner.SetAvatarMove(0, 0.125f, 0.25f);
    ASSERT_EQ(runner.RunFixedTicks(2).ticks_executed, 2u);
    const auto directory = runner.Session()->GetWorldSaveDir();
    const auto before = DiskBytes(directory);
    World::PlayerAvatarOwnerState owner;
    ASSERT_TRUE(runner.CaptureAvatarOwner(0, owner));
    SiblingWriteFailure failure;
    failure.ArmMetadata(directory);
    EXPECT_EQ(runner.SaveFullSnapshot(), 0u);
    failure.Reset();
    EXPECT_TRUE(failure.reached());
    EXPECT_EQ(DiskBytes(directory), before);
    World::PlayerAvatarOwnerState pending;
    ASSERT_TRUE(runner.CaptureAvatarOwner(0, pending));
    EXPECT_EQ(owner.physics.size, pending.physics.size);
    EXPECT_EQ(owner.physics.bytes, pending.physics.bytes);
    EXPECT_FALSE(Persistence::journal_pending(directory));
    EXPECT_GT(runner.SaveFullSnapshot(), 0u);
}

TEST_F(WorldOpenRefusal, OwnerContinuationTransientSnapshotsRetainOwnersAndMetadata) {
    auto original_metadata = nlohmann::json::parse(Read(save / "world_info.json"));
    original_metadata["extension"] = {{"unknown", {1, 2, 3}}};
    Write(save / "world_info.json", original_metadata.dump());
    std::vector<World::PlayerAvatar> avatars(1);
    GameSession source({.owner_continuation = true});
    source.SetJobSystem(&jobs);
    source.SetRootPath(RootString());
    source.SetActiveRegionsEnabled(false);
    source.ConfigureOwnerContinuation(avatars, 1, false, false);
    ASSERT_TRUE(source.CreateTransientWorld("Owner snapshot", "1337", "default"));
    auto* physics = source.GetPhysicsSystem();
    const auto index = physics->create_avatar_character(Vec3(8.25f, 40.125f, 8.75f));
    physics->set_avatar_wish_velocity(index, glm::vec2(0.125f, 0.25f));
    physics->update_avatars(1.0f / 30.0f);
    avatars[0].position = physics->get_avatar_position(index);
    avatars[0].velocity = physics->get_avatar_velocity(index);
    World::PlayerAvatarOwnerState expected;
    ASSERT_TRUE(source.CaptureAvatarOwner(avatars[0], index, expected));
    for (const auto& destination : {save, root / "owner-snapshot"}) {
        SCOPED_TRACE(destination.generic_string());
        ASSERT_TRUE(source.SaveWorldStateTo(destination, nullptr, true));
        const auto metadata = nlohmann::json::parse(Read(destination / "world_info.json"));
        ASSERT_TRUE(metadata.contains("owner_continuation"));
        if (destination == save) {
            EXPECT_EQ(metadata.at("name"), original_metadata.at("name"));
            EXPECT_EQ(metadata.at("creationTime"), original_metadata.at("creationTime"));
            EXPECT_EQ(metadata.at("extension"), original_metadata.at("extension"));
            EXPECT_EQ(metadata.at("seed"), original_metadata.at("seed"));
        }
        EXPECT_EQ(metadata.at("renderCalendar"),
                  world::WorldClock(0, source.GetRenderCalendar()).calendar_metadata());
        std::vector<World::PlayerAvatar> restored_avatars;
        GameSession restored({.owner_continuation = true});
        restored.SetJobSystem(&jobs);
        restored.SetRootPath(RootString());
        restored.SetActiveRegionsEnabled(false);
        restored.ConfigureOwnerContinuation(restored_avatars, 1, false, false);
        ASSERT_TRUE(restored.CreateTransientWorld("Owner reader", "1337", "default"));
        ASSERT_TRUE(restored.LoadWorldStateFrom(destination)) << restored.GetWorldOpenError();
        ASSERT_NE(restored.RestoredOwnerContinuation(), nullptr);
        ASSERT_EQ(restored.RestoredOwnerContinuation()->avatars.size(), 1u);
        World::PlayerAvatarOwnerState actual;
        ASSERT_TRUE(restored.CaptureAvatarOwner(
            restored.RestoredOwnerContinuation()->avatars[0].avatar, 0, actual));
        EXPECT_EQ(actual.physics.size, expected.physics.size);
        EXPECT_EQ(actual.physics.bytes, expected.physics.bytes);
        const auto* previous_physics = restored.GetPhysicsSystem();
        const auto creature = restored.GetRegistry().create();
        restored.GetRegistry().emplace<Luminumbra::Components::CreaturePhysicsComponent>(
            creature, std::size_t{0});
        const auto disk = DiskBytes(destination);
        EXPECT_FALSE(restored.LoadWorldStateFrom(destination));
        const auto prepared =
            GameSession::PrepareWorldState(restored.CaptureWorldPreparationOptions(), destination);
        EXPECT_FALSE(restored.AdoptPreparedWorld(prepared));
        EXPECT_EQ(restored.GetPhysicsSystem(), previous_physics);
        EXPECT_EQ(DiskBytes(destination), disk);
    }
    GameSession disabled;
    disabled.SetJobSystem(&jobs);
    disabled.SetRootPath(RootString());
    disabled.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(disabled.CreateTransientWorld("Disabled owner reader", "1337", "default"));
    const auto owner_bytes = DiskBytes(save);
    EXPECT_FALSE(disabled.LoadWorldStateFrom(save));
    EXPECT_FALSE(disabled.SaveWorldStateTo(save, nullptr, true));
    EXPECT_EQ(DiskBytes(save), owner_bytes);
    auto foreign_metadata = original_metadata;
    foreign_metadata["seed"] = "1338";
    const auto foreign_destination = root / "foreign-owner-snapshot";
    Write(foreign_destination / "world_info.json", foreign_metadata.dump());
    const auto foreign_bytes = DiskBytes(foreign_destination);
    EXPECT_FALSE(source.SaveWorldStateTo(foreign_destination, nullptr, true));
    EXPECT_EQ(DiskBytes(foreign_destination), foreign_bytes);
    // Admission covers both writer paths and refuses incomplete conversion
    // metadata, partial JSON, non-object input and unreadable metadata leaves.
    const auto invalid_destination = root / "invalid-owner-metadata";
    for (const auto& invalid :
         {std::string("{}"),
          std::string("{\"seed\":\"1337\",\"seedConversionVersion\":1,"
                      "\"resolvedSeed\":1337}"),
          std::string("{\"container_version\":2,\"seed\":"),
          std::string("[]"),
          std::string("{\"container_version\":2,\"seed\":\"\"}"),
          std::string("{\"container_version\":2,\"seed\":1337}"),
          std::string("{\"container_version\":2,\"seed\":\"1337\",\"resolvedSeed\":1337}"),
          std::string("{\"container_version\":2,\"seed\":\"1337\",\"seedConversionVersion\":1}"),
          std::string("{\"container_version\":2,\"seed\":\"1337\",\"seedConversionVersion\":2,"
                      "\"resolvedSeed\":1337}"),
          std::string("{\"container_version\":2,\"seed\":\"1337\",\"seedConversionVersion\":1,"
                      "\"resolvedSeed\":1338}"),
          std::string("{\"container_version\":2,\"seed\":\"1337\",\"seedConversionVersion\":1,"
                      "\"resolvedSeed\":4294967296}")}) {
        SCOPED_TRACE(invalid);
        Write(invalid_destination / "world_info.json", invalid);
        const auto invalid_bytes = DiskBytes(invalid_destination);
        EXPECT_FALSE(source.SaveWorldStateTo(invalid_destination, nullptr, true));
        EXPECT_EQ(DiskBytes(invalid_destination), invalid_bytes);
        EXPECT_FALSE(Persistence::journal_pending(invalid_destination));
    }
    fs::remove(invalid_destination / "world_info.json");
    fs::create_directory(invalid_destination / "world_info.json");
    const auto unreadable_bytes = DiskBytes(invalid_destination);
    EXPECT_FALSE(source.SaveWorldStateTo(invalid_destination, nullptr, true));
    EXPECT_EQ(DiskBytes(invalid_destination), unreadable_bytes);
    // Existing destinations require a supported container version, including
    // keyless metadata. Equivalent seeds and unrelated extensions survive.
    for (const auto& compatible :
         {nlohmann::json{{"container_version", WorldSaveService::kContainerVersion}},
          nlohmann::json{{"container_version", WorldSaveService::kContainerVersion},
                         {"seed", "1337"},
                         {"seedConversionVersion", world::kSeedConversionVersion},
                         {"resolvedSeed", 1337u},
                         {"extension", {"retained"}}}}) {
        const auto compatible_destination =
            root /
            (compatible.contains("seed") ? "versioned-owner-metadata" : "empty-owner-metadata");
        Write(compatible_destination / "world_info.json", compatible.dump());
        ASSERT_TRUE(source.SaveWorldStateTo(compatible_destination, nullptr, true));
        const auto written =
            nlohmann::json::parse(Read(compatible_destination / "world_info.json"));
        auto expected_metadata = compatible;
        expected_metadata["container_version"] = WorldSaveService::kIndexedMetadataVersion;
        for (const auto& item : expected_metadata.items()) {
            EXPECT_EQ(written.at(item.key()), item.value());
        }
    }
    // The input itself fits the existing metadata bound; adding the owner record
    // must refuse before payload publication, including for a keyless export.
    auto metadata = original_metadata;
    metadata["extension"] = "";
    const auto remaining = Persistence::SavedInputLimits::kMetadataBytes - metadata.dump().size();
    metadata["extension"] = std::string(remaining - 64u, 'x');
    const auto destination = root / "owner-metadata-bound";
    Write(destination / "world_info.json", metadata.dump());
    ASSERT_LE(Read(destination / "world_info.json").size(),
              Persistence::SavedInputLimits::kMetadataBytes);
    ASSERT_FALSE(Persistence::snapshot_index_exists(destination));
    const auto before = DiskBytes(destination);
    EXPECT_FALSE(source.SaveWorldStateTo(destination, nullptr, true));
    EXPECT_EQ(DiskBytes(destination), before);
    EXPECT_FALSE(Persistence::journal_pending(destination));
}

TEST_F(WorldOpenRefusal, OwnerContinuationRetainsCollisionFlagsIndependentlyOfTerrainBodies) {
    for (const bool body_present : {false, true}) {
        SCOPED_TRACE(body_present);
        GameSession source({.owner_continuation = true});
        source.SetJobSystem(&jobs);
        source.SetRootPath(RootString());
        source.SetActiveRegionsEnabled(false);
        ASSERT_TRUE(source.CreateTransientWorld("Collision flags", "1337", "default"));
        auto chunk = std::make_shared<Chunk>(IVec3(0, 0, 0));
        chunk->set_state(ChunkState::Idle);
        chunk->heightmap_data.assign(static_cast<std::size_t>(CHUNK_SIZE_X + 1) *
                                         (CHUNK_SIZE_Z + 1),
                                     body_present ? 8.0f : -1.0f);
        ASSERT_TRUE(source.GetPhysicsSystem()->add_chunk_collision(*chunk));
        const auto body = source.GetPhysicsSystem()->chunk_collision_body(chunk->get_id());
        ASSERT_EQ(body.has_value(), body_present);
        // No body is needed outside the chunk's height interval. Conversely a
        // remesh may leave collision work pending while the old body still exists.
        chunk->has_collision.store(!body_present);
        ASSERT_TRUE(source.GetWorldSystem()->adopt_streamed_chunk(chunk));
        ASSERT_TRUE(source.SaveWorldStateTo(save, nullptr, true));
        const auto expected = source.ComputeChunkStateHash();
        GameSession restored({.owner_continuation = true});
        restored.SetJobSystem(&jobs);
        restored.SetRootPath(RootString());
        restored.SetActiveRegionsEnabled(false);
        ASSERT_TRUE(restored.CreateTransientWorld("Collision reader", "1337", "default"));
        ASSERT_TRUE(restored.LoadWorldStateFrom(save)) << restored.GetWorldOpenError();
        const auto loaded = restored.GetWorldSystem()->find_streamed_chunk(chunk->get_coords());
        ASSERT_NE(loaded, nullptr);
        EXPECT_EQ(loaded->has_collision.load(), chunk->has_collision.load());
        EXPECT_EQ(restored.GetPhysicsSystem()->chunk_collision_body(chunk->get_id()), body);
        EXPECT_EQ(restored.ComputeChunkStateHash(), expected);
    }
}

TEST_F(WorldOpenRefusal, OwnerContinuationPersistentSnapshotRefusesForeignSeedBeforeWriting) {
    GameSession source({.owner_continuation = true});
    source.SetJobSystem(&jobs);
    source.SetRootPath(RootString());
    source.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(source.CreateWorld("Owner snapshot", "1337", "default"));
    auto foreign = nlohmann::json::parse(Read(source.GetWorldSaveDir() / "world_info.json"));
    foreign["seed"] = "1338";
    foreign["resolvedSeed"] = 1338u;
    const auto destination = root / "foreign-persistent-owner-snapshot";
    Write(destination / "world_info.json", foreign.dump());
    const auto bytes = DiskBytes(root);
    EXPECT_FALSE(source.SaveWorldStateTo(destination, nullptr, true));
    EXPECT_EQ(DiskBytes(root), bytes);
    EXPECT_FALSE(Persistence::journal_pending(destination));
}

TEST_F(WorldOpenRefusal, OwnerContinuationRefusesUnboundControllersBeforeWriting) {
    GameSession source({.owner_continuation = true});
    source.SetJobSystem(&jobs);
    source.SetRootPath(RootString());
    source.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(source.CreateTransientWorld("Unbound owners", "1337", "default"));
    const auto before = DiskBytes(save);
    ASSERT_NE(source.GetPhysicsSystem()->create_avatar_character({8, 40, 8}),
              Systems::PhysicsSystem::kInvalidAvatar);
    EXPECT_FALSE(source.SaveWorldStateTo(save, nullptr, true));
    EXPECT_EQ(DiskBytes(save), before);
}

TEST_F(WorldOpenRefusal, OwnerContinuationPreparedRosterChangeRefusesWithoutPublication) {
    std::vector<World::PlayerAvatar> avatars;
    GameSession live({.owner_continuation = true});
    live.SetJobSystem(&jobs);
    live.SetRootPath(RootString());
    ASSERT_TRUE(live.LoadWorld("fixture"));
    ASSERT_TRUE(live.SaveWorldState());
    const auto prepared =
        GameSession::PrepareWorldLoad(live.CaptureWorldPreparationOptions(), "fixture");
    ASSERT_TRUE(GameSession::WorldPreparationError(*prepared).empty());
    const auto* world = live.GetWorldSystem();
    const auto* physics = live.GetPhysicsSystem();
    const auto revision = live.WorldRevision();
    const auto bytes = DiskBytes(root);
    live.ConfigureOwnerContinuation(avatars, 1, false, false);
    EXPECT_FALSE(live.AdoptPreparedWorld(prepared));
    EXPECT_EQ(live.GetWorldSystem(), world);
    EXPECT_EQ(live.GetPhysicsSystem(), physics);
    EXPECT_EQ(live.WorldRevision(), revision);
    EXPECT_EQ(DiskBytes(root), bytes);
}

} // namespace

TEST_F(WorldOpenRefusal, OverlappingSessionSaveOwnsDestinationThroughMetadataAndRefusal) {
    // Four cases bound this pass: incremental/complete exports, each with a
    // compatible or refused clock configuration. Each session has one chunk.
    for (const bool full_snapshot : {false, true}) {
        for (const bool compatible : {false, true}) {
            SCOPED_TRACE(full_snapshot);
            SCOPED_TRACE(compatible);
            const auto first_config = SnapshotRunnerConfig(RootString(), true);
            auto second_config = SnapshotRunnerConfig(RootString(), compatible);
            second_config.world_name = "Other snapshot";
            Server::ServerWorldRunner first(first_config);
            Server::ServerWorldRunner second(second_config);
            ASSERT_NO_FATAL_FAILURE(BootSnapshotRunner(first, true));
            ASSERT_NO_FATAL_FAILURE(BootSnapshotRunner(second, compatible));
            std::shared_ptr<Chunk> first_chunk;
            std::shared_ptr<Chunk> second_chunk;
            ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(*first.Session(), first_chunk, true));
            ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(*second.Session(), second_chunk, true));
            second_chunk->heightmap_data.front() = 84.0f;
            second.Session()->SetSpawnPoint(Vec3(29, 31, 37));
            const PendingSnapshotState first_pending(*first.Session());
            const PendingSnapshotState second_pending(*second.Session());
            const auto destination = first.Session()->GetWorldSaveDir();
            SessionSaveOverlap overlap;
            ActiveSessionSaveOverlap() = &overlap;
            WorldSaveService::set_before_active_regions_replace_for_testing(&HoldSessionMetadata);
            bool first_saved = false;
            bool second_saved = false;
            std::atomic<bool> second_done{false};
            std::jthread first_writer([&] { first_saved = first.Session()->SaveWorld(); });
            overlap.staged.arrive_and_wait();
            std::jthread second_writer([&] {
                second_saved =
                    second.Session()->SaveWorldStateTo(destination, nullptr, full_snapshot);
                second_done.store(true, std::memory_order_release);
            });
            // Ownership wait or return is observable, so no sleeps or assumed
            // scheduling order are needed. On an unguarded session save the
            // incompatible call returns here while the first save is incomplete.
            while (WorldSaveService::waiting_save_writers() == 0 &&
                   !second_done.load(std::memory_order_acquire))
                std::this_thread::yield();
            EXPECT_FALSE(second_done.load(std::memory_order_acquire));
            EXPECT_EQ(WorldSaveService::waiting_save_writers(), 1u);
            // The hook remains installed until both writers finish; only its
            // first invocation pauses, so an unexpected refusal cannot strand a barrier.
            overlap.release.arrive_and_wait();
            first_writer.join();
            second_writer.join();
            WorldSaveService::set_before_active_regions_replace_for_testing(nullptr);
            ActiveSessionSaveOverlap() = nullptr;
            EXPECT_TRUE(first_saved);
            EXPECT_EQ(second_saved, compatible);
            EXPECT_EQ(WorldSaveService::waiting_save_writers(), 0u);
            EXPECT_TRUE(second_chunk->is_voxel_data_dirty());
            const auto metadata = nlohmann::json::parse(Read(destination / "world_info.json"));
            const auto* expected_session = compatible ? second.Session() : first.Session();
            const auto& expected_metadata = expected_session->GetMetadata();
            EXPECT_EQ(metadata.at("name"), expected_metadata.name);
            EXPECT_EQ(metadata.at("spawnPoint").at("x"), expected_metadata.spawnPoint.x);
            EXPECT_EQ(metadata.at("spawnPoint").at("y"), expected_metadata.spawnPoint.y);
            EXPECT_EQ(metadata.at("spawnPoint").at("z"), expected_metadata.spawnPoint.z);
            ASSERT_NO_FATAL_FAILURE(
                ExpectSnapshotReloaded(compatible ? second_pending : first_pending,
                                       compatible ? *second_chunk : *first_chunk,
                                       first_config,
                                       first.Session()->GetMetadata().worldId,
                                       jobs));
            // A refused writer releases ownership and can save to its own root.
            ASSERT_TRUE(second.Session()->SaveWorld());
            first.Shutdown();
            second.Shutdown();
        }
    }
}

TEST_F(WorldOpenRefusal, ClientExitCompleteSavePublishesCapturedSpawnInBothRegionModes) {
    for (const bool regions : {false, true}) {
        SCOPED_TRACE(regions);
        const auto config = SnapshotRunnerConfig(RootString(), regions);
        Server::ServerWorldRunner runner(config);
        ASSERT_NO_FATAL_FAILURE(BootSnapshotRunner(runner, regions));
        auto* session = runner.Session();
        std::shared_ptr<Chunk> chunk;
        ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(*session, chunk, true));
        const PendingSnapshotState pending(*session);
        const Vec3 spawn(29, 31, 37);
        bool reported = false;
        Client::SaveSessionOnExit(
            session,
            true,
            [&] { session->SetSpawnPoint(spawn); },
            [&] {
                reported = true;
                const auto metadata =
                    nlohmann::json::parse(Read(session->GetWorldSaveDir() / "world_info.json"));
                EXPECT_EQ(metadata.at("spawnPoint").at("x"), spawn.x);
                EXPECT_EQ(metadata.at("spawnPoint").at("y"), spawn.y);
                EXPECT_EQ(metadata.at("spawnPoint").at("z"), spawn.z);
                EXPECT_FALSE(chunk->is_voxel_data_dirty());
            });
        ASSERT_TRUE(reported);
        ASSERT_NO_FATAL_FAILURE(
            ExpectSnapshotReloaded(pending, *chunk, config, session->GetMetadata().worldId, jobs));
        runner.Shutdown();
    }
}

class RollbackProgressObservation {
public:
    RollbackProgressObservation(const fs::path& directory,
                                const std::string& chunks,
                                const std::string& plants,
                                bool energy_failure,
                                const std::shared_ptr<Chunk>& chunk)
        : directory_(directory)
        , chunks_(chunks)
        , plants_(plants)
        , energy_failure_(energy_failure)
        , chunk_(chunk) {
        current() = this;
        Luminumbra::Persistence::set_snapshot_step_hook_for_testing(&Observe);
    }
    ~RollbackProgressObservation() {
        Luminumbra::Persistence::set_snapshot_step_hook_for_testing(nullptr);
        current() = nullptr;
    }
    bool observed = false;
    Luminumbra::u32 voxel_revision = 0;
    Luminumbra::u32 water_revision = 0;

private:
    static RollbackProgressObservation*& current() {
        static RollbackProgressObservation* observation = nullptr;
        return observation;
    }
    static void Observe(const char* step, const fs::path& directory) {
        auto* observation = current();
        if (!observation || std::string_view(step) != "before-rollback" ||
            directory != observation->directory_)
            return;
        // This event runs after writer scopes and publication stripes have unwound.
        observation->observed = true;
        EXPECT_NE(Read(WorldSaveService::region_file_path(directory, 0, 0)), observation->chunks_);
        if (observation->energy_failure_) {
            EXPECT_NE(Read(WorldSaveService::plant_entities_path(directory)), observation->plants_);
        } else {
            EXPECT_EQ(Read(WorldSaveService::plant_entities_path(directory)), observation->plants_);
        }
        observation->voxel_revision = observation->chunk_->voxel_revision();
        observation->water_revision = observation->chunk_->water_revision();
    }
    fs::path directory_;
    std::string chunks_;
    std::string plants_;
    bool energy_failure_;
    std::shared_ptr<Chunk> chunk_;
};

void ExpectCompleteSaveFailure(int save_path,
                               world::GameSession* session,
                               Server::ServerWorldRunner& runner,
                               const fs::path& destination,
                               world::WorldStateSaveReport& report) {
    switch (save_path) {
        case 0:
            EXPECT_FALSE(session->SaveWorldState(&report));
            break;
        case 1:
            EXPECT_EQ(runner.SaveFullSnapshot(), 0u);
            break;
        case 2:
            EXPECT_FALSE(session->SaveWorld());
            break;
        case 3:
            EXPECT_FALSE(session->SaveWorldStateTo(destination, &report, /*full_snapshot=*/true));
            break;
        case 4: {
            bool spawn_captured = false;
            Client::SaveSessionOnExit(
                session,
                true,
                [&] {
                    spawn_captured = true;
                    session->SetSpawnPoint(Vec3(29, 31, 37));
                },
                [] { ADD_FAILURE() << "Failed exit save reported success"; });
            EXPECT_TRUE(spawn_captured);
            break;
        }
        case 5: {
            const auto tick_report = runner.RunFixedTicks(1);
            EXPECT_EQ(tick_report.ticks_executed, 1u);
            EXPECT_EQ(tick_report.autosave_passes, 0u);
            EXPECT_EQ(tick_report.autosave_writes, 0u);
            break;
        }
        case 6:
            runner.Shutdown(&report);
            break;
        default:
            FAIL() << "Unexpected save path " << save_path;
    }
}

void ExpectRetryAcknowledgesSave(int save_path,
                                 world::GameSession* session,
                                 Server::ServerWorldRunner& runner,
                                 const fs::path& canonical,
                                 const fs::path& destination,
                                 const std::shared_ptr<Chunk>& chunk,
                                 const std::string& old_metadata,
                                 const std::string& old_chunks) {
    if (save_path == 3) {
        ASSERT_TRUE(session->SaveWorldStateTo(destination, nullptr, /*full_snapshot=*/true));
    } else {
        ASSERT_TRUE(session->SaveWorld());
    }
    EXPECT_EQ(chunk->is_voxel_data_dirty(), save_path == 3);
    EXPECT_NE(Read(destination / "world_info.json"), old_metadata);
    if (save_path == 3) {
        // A successful export must not acknowledge the canonical save's edits.
        EXPECT_EQ(Read(canonical / "world_info.json"), old_metadata);
        EXPECT_EQ(Read(WorldSaveService::region_file_path(canonical, 0, 0)), old_chunks);
        ASSERT_TRUE(session->SaveWorldState());
        EXPECT_FALSE(chunk->is_voxel_data_dirty());
        EXPECT_EQ(Read(WorldSaveService::region_file_path(canonical, 0, 0)),
                  Read(WorldSaveService::region_file_path(destination, 0, 0)));
    }
    runner.Shutdown();
}

TEST_P(CompleteSaveFailure, SiblingWriterFailureKeepsMetadataAndDirtyChunksPending) {
    const auto [regions, failure_and_path] = GetParam();
    const bool energy_failure = failure_and_path % 2 != 0;
    const int save_path = failure_and_path / 2;
    auto config = SnapshotRunnerConfig(RootString(), regions == 1);
    if (save_path == 5)
        config.autosave_interval_ticks = 1;
    Server::ServerWorldRunner runner(config);
    ASSERT_NO_FATAL_FAILURE(BootSnapshotRunner(runner, regions == 1));
    auto* session = runner.Session();
    std::shared_ptr<Chunk> chunk;
    ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(*session, chunk, true));
    ASSERT_TRUE(session->SaveWorldState());
    const auto canonical = session->GetWorldSaveDir();
    if (regions == 2) {
        ASSERT_TRUE(Luminumbra::Test::PublishIndexedSave(canonical, [] { return true; }));
    }
    const auto destination = save_path == 3
                                 ? root / "alternate/worlds/saves" / session->GetMetadata().worldId
                                 : canonical;
    if (save_path == 3) {
        fs::create_directories(destination.parent_path());
        fs::copy(canonical, destination, fs::copy_options::recursive);
    }
    const auto committed_tree = DiskBytes(destination);
    const auto old_metadata = Read(destination / "world_info.json");
    const auto old_ledger = Read(WorldSaveService::active_regions_path(destination));
    const auto old_plants = Read(WorldSaveService::plant_entities_path(destination));
    const auto old_energy = Read(destination / "aether_state.efs");
    const auto old_chunks = Read(WorldSaveService::region_file_path(destination, 0, 0));
    session->SetSpawnPoint(Vec3(21, 22, 23));
    chunk->heightmap_data.front() = 47.0f;
    chunk->mark_voxel_data_dirty();
    auto plants = session->GetRegistry().view<Components::PlantGrowthComponent>();
    for (auto plant : plants)
        plants.get<Components::PlantGrowthComponent>(plant).growth_points += 7;
    session->GetEnergyFieldState()->QueueDeposit(7, 0, 0, 0, 500);
    ASSERT_NO_FATAL_FAILURE(ApplySnapshotTestEnergy(*session, 0));

    // Declare after the runner so even a fatal assertion releases the obstruction
    // before its destructor attempts a shutdown save.
    SiblingWriteFailure failure;
    ASSERT_NO_FATAL_FAILURE(failure.Arm(destination, energy_failure));
    std::unique_ptr<RollbackProgressObservation> observation;
    if (regions != 0)
        observation = std::make_unique<RollbackProgressObservation>(
            destination, old_chunks, old_plants, energy_failure, chunk);
    world::WorldStateSaveReport report;
    ASSERT_NO_FATAL_FAILURE(
        ExpectCompleteSaveFailure(save_path, session, runner, destination, report));
    EXPECT_TRUE(failure.reached());
    failure.Reset();
    EXPECT_FALSE(report.saved);
    EXPECT_EQ(report.chunks_saved, 0u);
    EXPECT_TRUE(chunk->is_voxel_data_dirty());
    EXPECT_EQ(Read(destination / "world_info.json"), old_metadata);
    EXPECT_EQ(Read(WorldSaveService::active_regions_path(destination)), old_ledger);
    if (regions != 0) {
        ASSERT_NE(observation, nullptr);
        EXPECT_TRUE(observation->observed);
        EXPECT_EQ(chunk->voxel_revision(), observation->voxel_revision);
        EXPECT_EQ(chunk->water_revision(), observation->water_revision);
        EXPECT_EQ(DiskBytes(destination), committed_tree);
        EXPECT_EQ(Read(WorldSaveService::region_file_path(destination, 0, 0)), old_chunks);
        EXPECT_EQ(Read(WorldSaveService::plant_entities_path(destination)), old_plants);
    } else {
        // Ordinary unindexed saves retain durable partial progress after sibling failure.
        EXPECT_NE(Read(WorldSaveService::region_file_path(destination, 0, 0)), old_chunks);
        if (energy_failure) {
            EXPECT_NE(Read(WorldSaveService::plant_entities_path(destination)), old_plants);
        } else {
            EXPECT_EQ(Read(WorldSaveService::plant_entities_path(destination)), old_plants);
        }
    }
    EXPECT_EQ(Read(destination / "aether_state.efs"), old_energy);
    observation.reset();
    EXPECT_TRUE(WorldSaveService::validate_save(destination));
    if (save_path != 6) {
        ASSERT_NO_FATAL_FAILURE(ExpectRetryAcknowledgesSave(
            save_path, session, runner, canonical, destination, chunk, old_metadata, old_chunks));
    }
}

// Each CTest entry exercises one writer, host entry point and region mode.
INSTANTIATE_TEST_SUITE_P(RegionModesWritersAndSavePaths,
                         CompleteSaveFailure,
                         testing::Combine(testing::Values(0, 1, 2), testing::Range(0, 14)));

TEST_F(WorldOpenRefusal, FailedScheduledAutosaveRetriesAfterFixedTickBackoff) {
    constexpr std::uint64_t kInterval = 100;
    static_assert(Server::kAutosaveRetryTicks > 1 && Server::kAutosaveRetryTicks < kInterval);
    auto config = SnapshotRunnerConfig(RootString(), true);
    config.autosave_interval_ticks = kInterval;
    Server::ServerWorldRunner runner(config);
    ASSERT_NO_FATAL_FAILURE(BootSnapshotRunner(runner, true));
    auto* session = runner.Session();
    std::shared_ptr<Chunk> chunk;
    ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(*session, chunk, true));
    const auto save_dir = session->GetWorldSaveDir();
    const double dt = session->GetSimulationClock().fixed_dt();
    while ((session->GetSimulationTickCount() + 1) % kInterval != 0)
        ASSERT_EQ(session->TickSimulation(dt), 1u);
    const std::uint64_t boundary = session->GetSimulationTickCount() + 1;

    SiblingWriteFailure failure;
    ASSERT_NO_FATAL_FAILURE(failure.Arm(save_dir, true));
    auto tick_report = runner.RunFixedTicks(1);
    EXPECT_TRUE(failure.reached());
    failure.Reset();
    EXPECT_EQ(tick_report.autosave_passes, 0u);
    EXPECT_TRUE(chunk->is_voxel_data_dirty());

    tick_report = runner.RunFixedTicks(Server::kAutosaveRetryTicks - 1);
    EXPECT_EQ(tick_report.autosave_passes, 0u);
    EXPECT_TRUE(chunk->is_voxel_data_dirty());
    tick_report = runner.RunFixedTicks(1);
    EXPECT_EQ(tick_report.autosave_passes, 1u);
    EXPECT_FALSE(chunk->is_voxel_data_dirty());
    EXPECT_EQ(Persistence::InspectSavedWorld(root, session->GetMetadata().worldId).clock.tick(),
              boundary + Server::kAutosaveRetryTicks);
    EXPECT_TRUE(WorldSaveService::validate_save(save_dir));
    runner.Shutdown();
}

TEST_F(WorldOpenRefusal, IndexedFlagOffFinalMetadataFailureRollsBackEnergyAndEmptyPlants) {
    for (const bool remove_energy : {false, true}) {
        SCOPED_TRACE(remove_energy);
        Server::ServerWorldRunner runner(SnapshotRunnerConfig(RootString(), false));
        ASSERT_NO_FATAL_FAILURE(BootSnapshotRunner(runner, false));
        auto* session = runner.Session();
        std::shared_ptr<Chunk> chunk;
        ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(*session, chunk, true));
        ASSERT_TRUE(session->SaveWorldState());
        const auto destination = session->GetWorldSaveDir();
        ASSERT_TRUE(Luminumbra::Test::PublishIndexedSave(destination, [] { return true; }));
        const auto baseline = DiskBytes(destination);
        const auto old_chunks = Read(WorldSaveService::region_file_path(destination, 0, 0));
        const auto old_plants = Read(WorldSaveService::plant_entities_path(destination));
        ASSERT_FALSE(old_plants.empty());
        std::vector<entt::entity> plants;
        for (const auto entity : session->GetRegistry().view<Components::PlantGrowthComponent>())
            plants.push_back(entity);
        for (const auto entity : plants)
            session->GetRegistry().destroy(entity);
        if (remove_energy) {
            session->GetEnergyFieldState()->ResetEmptyAtTick(session->GetSimulationTickCount());
        } else {
            session->GetEnergyFieldState()->QueueDeposit(7, 0, 0, 0, 500);
            ASSERT_NO_FATAL_FAILURE(ApplySnapshotTestEnergy(*session, 0));
        }
        chunk->heightmap_data.front() = 47.0f;
        chunk->mark_voxel_data_dirty();
        session->SetSpawnPoint(Vec3(21, 22, 23));
        SiblingWriteFailure failure;
        failure.ArmMetadata(destination);
        {
            RollbackProgressObservation observation(
                destination, old_chunks, old_plants, true, chunk);
            EXPECT_FALSE(session->SaveWorld());
            EXPECT_TRUE(observation.observed);
            EXPECT_EQ(chunk->voxel_revision(), observation.voxel_revision);
            EXPECT_EQ(chunk->water_revision(), observation.water_revision);
        }
        EXPECT_TRUE(failure.reached());
        failure.Reset();
        EXPECT_EQ(DiskBytes(destination), baseline);
        EXPECT_TRUE(chunk->is_voxel_data_dirty());
        EXPECT_FALSE(Persistence::journal_pending(destination));
        EXPECT_TRUE(WorldSaveService::validate_save(destination));
        ASSERT_TRUE(session->SaveWorld());
        EXPECT_FALSE(chunk->is_voxel_data_dirty());
        EXPECT_FALSE(fs::exists(WorldSaveService::plant_entities_path(destination)));
        EXPECT_EQ(fs::exists(destination / "aether_state.efs"), !remove_energy);
        runner.Shutdown();
    }
}

TEST_F(WorldOpenRefusal, IndexedEmptyPlantRemovalPreservesSubstitutedOccupantAndJournal) {
    for (const bool regions : {false, true}) {
        SCOPED_TRACE(regions);
        const auto config = SnapshotRunnerConfig(RootString(), regions);
        Server::ServerWorldRunner runner(config);
        ASSERT_NO_FATAL_FAILURE(BootSnapshotRunner(runner, regions));
        auto* session = runner.Session();
        std::shared_ptr<Chunk> chunk;
        ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(*session, chunk, true));
        ASSERT_TRUE(session->SaveWorldState());
        const auto destination = session->GetWorldSaveDir();
        if (!regions) {
            ASSERT_TRUE(Luminumbra::Test::PublishIndexedSave(destination, [] { return true; }));
        }
        const auto baseline = DiskBytes(destination);
        const auto path = WorldSaveService::plant_entities_path(destination);
        ASSERT_FALSE(Read(path).empty());
        const auto suffix = regions ? "enabled" : "disabled";
        const auto displaced = root / (std::string("owned-plant-") + suffix);
        const auto foreign = root / (std::string("foreign-plant-") + suffix);
        SiblingWriteFailure substitution;
        substitution.ArmPlantRemoval(destination, displaced);
        std::vector<std::string> errors;
        EXPECT_FALSE(WorldSaveService::save_plant_entities({}, destination, &errors));
        EXPECT_TRUE(substitution.reached());
        substitution.Reset();
        EXPECT_FALSE(errors.empty());
        EXPECT_EQ(Read(path), "foreign plant occupant");
        EXPECT_TRUE(Persistence::journal_pending(destination));
        fs::rename(path, foreign);
        fs::rename(displaced, path);
        EXPECT_EQ(Persistence::recover_pending_journal(destination, nullptr),
                  Persistence::RecoveryOutcome::RolledBack);
        EXPECT_EQ(DiskBytes(destination), baseline);
        EXPECT_EQ(Read(foreign), "foreign plant occupant");
        EXPECT_FALSE(Persistence::journal_pending(destination));
        runner.Shutdown();
    }
}

TEST_F(WorldOpenRefusal, CanonicalSaveWithTrailingSeparatorAcknowledgesPendingEdits) {
    for (const bool regions : {false, true}) {
        SCOPED_TRACE(regions);
        const auto config = SnapshotRunnerConfig(RootString(), regions);
        Server::ServerWorldRunner runner(config);
        ASSERT_NO_FATAL_FAILURE(BootSnapshotRunner(runner, regions));
        auto* session = runner.Session();
        std::shared_ptr<Chunk> chunk;
        ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(*session, chunk, true));
        const auto destination = session->GetWorldSaveDir() / "";
        ASSERT_TRUE(destination.filename().empty());
        const PendingSnapshotState pending(*session);
        world::WorldStateSaveReport report;
        ASSERT_TRUE(session->SaveWorldStateTo(destination, &report));
        EXPECT_TRUE(report.saved);
        EXPECT_FALSE(chunk->is_voxel_data_dirty());
        ASSERT_NO_FATAL_FAILURE(
            ExpectSnapshotReloaded(pending, *chunk, config, session->GetMetadata().worldId, jobs));
        runner.Shutdown();
    }
}

TEST_F(WorldOpenRefusal, SaveAsWithTrailingSeparatorRetainsCanonicalPendingEdits) {
    for (const bool regions : {false, true}) {
        SCOPED_TRACE(regions);
        const auto config = SnapshotRunnerConfig(RootString(), regions);
        Server::ServerWorldRunner runner(config);
        ASSERT_NO_FATAL_FAILURE(BootSnapshotRunner(runner, regions));
        auto* session = runner.Session();
        std::shared_ptr<Chunk> chunk;
        ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(*session, chunk, true));
        const auto canonical = session->GetWorldSaveDir();
        const auto before = Read(canonical / "world_info.json");
        const auto before_chunks = Read(WorldSaveService::region_file_path(canonical, 0, 0));
        if (regions) {
            ASSERT_EQ(session->TickSimulation(session->GetSimulationClock().fixed_dt()), 1u);
        }
        const auto ledger = session->GetActiveRegionLedger().encode();
        Write(canonical / "unrelated/retained.txt", "canonical member");
        const auto destination = root / (regions ? "enabled-export" : "legacy-export") / "";
        ASSERT_TRUE(destination.filename().empty());
        world::WorldStateSaveReport report;
        ASSERT_TRUE(session->SaveWorldStateTo(destination, &report, /*full_snapshot=*/true));
        EXPECT_TRUE(report.saved);
        EXPECT_TRUE(chunk->is_voxel_data_dirty());
        EXPECT_EQ(session->GetActiveRegionLedger().encode(), ledger);
        EXPECT_EQ(Read(canonical / "world_info.json"), before);
        EXPECT_EQ(Read(WorldSaveService::region_file_path(canonical, 0, 0)), before_chunks);
        EXPECT_EQ(Read(canonical / "unrelated/retained.txt"), "canonical member");
        EXPECT_FALSE(fs::exists(destination / "unrelated"));
        EXPECT_TRUE(WorldSaveService::validate_save(destination));
        WorldStreamingState exported;
        std::vector<std::string> errors;
        ASSERT_TRUE(WorldSaveService{}.load_world(exported, destination, errors));
        EXPECT_TRUE(errors.empty());
        ASSERT_EQ(exported.size(), 1u);
        ASSERT_NE(exported.find_chunk(chunk->get_id()), nullptr);
        EXPECT_EQ(exported.find_chunk(chunk->get_id())->heightmap_data, chunk->heightmap_data);
        ASSERT_TRUE(session->SaveWorldState());
        EXPECT_FALSE(chunk->is_voxel_data_dirty());
        EXPECT_EQ(Read(WorldSaveService::region_file_path(canonical, 0, 0)),
                  Read(WorldSaveService::region_file_path(destination, 0, 0)));
        runner.Shutdown();
    }
}

TEST_F(WorldOpenRefusal, PartialChunkReplacementKeepsMetadataAndDirtyChunksPending) {
    const auto config = SnapshotRunnerConfig(RootString(), true);
    Server::ServerWorldRunner runner(config);
    ASSERT_NO_FATAL_FAILURE(BootSnapshotRunner(runner, true));
    auto* session = runner.Session();
    std::shared_ptr<Chunk> chunk;
    ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(*session, chunk, true));
    ASSERT_TRUE(session->SaveWorldState());
    const auto destination = session->GetWorldSaveDir();
    const auto metadata = Read(destination / "world_info.json");
    const auto plants = Read(WorldSaveService::plant_entities_path(destination));
    const auto energy = Read(destination / "aether_state.efs");
    auto second = std::make_shared<Chunk>(IVec3(32, 0, 0));
    second->set_state(ChunkState::Idle);
    second->heightmap_data = {17.0f};
    second->mark_voxel_data_dirty();
    ASSERT_TRUE(session->GetWorldSystem()->adopt_streamed_chunk(second));
    session->SetSpawnPoint(Vec3(29, 31, 37));
    chunk->heightmap_data.front() = 47.0f;
    chunk->mark_voxel_data_dirty();
    // Refuse an invalid second region before making any replacements.
    const auto blocked = WorldSaveService::region_file_path(destination, 1, 0);
    fs::create_directory(blocked);
    Write(blocked / "keep", "occupied");
    // The initial integrity scan correctly refuses an invalid region entry.
    EXPECT_FALSE(session->SaveWorldState());
    EXPECT_TRUE(chunk->is_voxel_data_dirty());
    EXPECT_TRUE(second->is_voxel_data_dirty());
    EXPECT_EQ(Read(destination / "world_info.json"), metadata);
    EXPECT_EQ(Read(WorldSaveService::plant_entities_path(destination)), plants);
    EXPECT_EQ(Read(destination / "aether_state.efs"), energy);
    fs::remove_all(blocked);
    const auto authority_revision = session->GetWorldSystem()->far_lod_authority_revision();
#if defined(_WIN32)
    WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
#else
    // A post-replacement flush failure leaves the new complete region in place.
    int ancestor_flushes = 0;
    for (auto ancestor = WorldSaveService::region_directory(destination);
         !ancestor.empty() && ancestor != ancestor.root_path();
         ancestor = ancestor.parent_path())
        ++ancestor_flushes;
    WorldSaveService::set_directory_flush_failure_for_testing(ancestor_flushes);
#endif
    EXPECT_FALSE(session->SaveWorldState());
    WorldSaveService::set_directory_flush_failure_for_testing(-1);
    WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
    EXPECT_GT(session->GetWorldSystem()->far_lod_authority_revision(), authority_revision);
    EXPECT_TRUE(chunk->is_voxel_data_dirty());
    EXPECT_TRUE(second->is_voxel_data_dirty());
    EXPECT_EQ(Read(destination / "world_info.json"), metadata);
    EXPECT_TRUE(WorldSaveService::validate_save(destination));
    ASSERT_TRUE(session->SaveWorldState());
    EXPECT_FALSE(chunk->is_voxel_data_dirty());
    EXPECT_FALSE(second->is_voxel_data_dirty());
    runner.Shutdown();
}

TEST_F(WorldOpenRefusal, EmptyEnergyRemovalFailureKeepsMetadataInBothRegionModes) {
    for (const bool regions : {false, true}) {
        const auto config = SnapshotRunnerConfig(RootString(), regions);
        Server::ServerWorldRunner runner(config);
        ASSERT_NO_FATAL_FAILURE(BootSnapshotRunner(runner, regions));
        auto* session = runner.Session();
        std::shared_ptr<Chunk> chunk;
        ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(*session, chunk, true));
        ASSERT_TRUE(session->SaveWorldState());
        const auto destination = session->GetWorldSaveDir();
        const auto metadata = Read(destination / "world_info.json");
        const auto energy = destination / "aether_state.efs";
        const auto abandoned = fs::path(energy.string() + ".tmp.abandoned");
        Write(abandoned, "partial field snapshot");
        // Nonempty saves reclaim abandoned energy before publishing metadata.
        ASSERT_TRUE(session->SaveWorldState());
        EXPECT_FALSE(fs::exists(abandoned));
        const auto preserved = root / "previous-energy";
        fs::rename(energy, preserved);
        fs::create_directory(energy);
        Write(energy / "keep", "occupied");
        session->GetEnergyFieldState()->ResetEmptyAtTick(session->GetSimulationTickCount());
        Write(abandoned, "partial field snapshot");
        session->SetSpawnPoint(Vec3(29, 31, 37));
        chunk->mark_voxel_data_dirty();
        EXPECT_FALSE(session->SaveWorldState());
        EXPECT_TRUE(chunk->is_voxel_data_dirty());
        EXPECT_EQ(Read(destination / "world_info.json"), metadata);
        EXPECT_EQ(Read(energy / "keep"), "occupied");
        EXPECT_FALSE(fs::exists(abandoned));
        Write(abandoned, "partial field snapshot");
        fs::remove_all(energy);
        fs::rename(preserved, energy);
        ASSERT_TRUE(session->SaveWorldState());
        EXPECT_FALSE(fs::exists(energy));
        EXPECT_FALSE(fs::exists(abandoned));
        EXPECT_FALSE(chunk->is_voxel_data_dirty());
        runner.Shutdown();
    }
}

TEST_F(WorldOpenRefusal, EmptyLegacyEnergySavePreservesRebasedCadence) {
    const auto config = SnapshotRunnerConfig(RootString(), false);
    ASSERT_TRUE(config.systems_config_override.has_value());
    if (!config.systems_config_override.has_value())
        return;
    Server::ServerWorldRunner runner(config);
    ASSERT_NO_FATAL_FAILURE(BootSnapshotRunner(runner, false));
    auto* session = runner.Session();
    ASSERT_EQ(session->TickSimulation(session->GetSimulationClock().fixed_dt()), 1u);
    ASSERT_EQ(session->GetEnergyFieldState()->page_count(), 0u);
    ASSERT_TRUE(session->SaveWorldState());
    EXPECT_EQ(Read(session->GetWorldSaveDir() / "aether_state.efs"), "EFS1 2 7\n");
    GameSession loaded;
    loaded.SetRootPath(RootString());
    loaded.SetJobSystem(&jobs);
    loaded.ApplySystemConfig(*config.systems_config_override);
    ASSERT_TRUE(loaded.LoadWorld(session->GetMetadata().worldId));
    EXPECT_EQ(loaded.GetSimulationTickCount(), 0u);
    EXPECT_EQ(loaded.GetEnergyFieldState()->page_count(), 0u);
    EXPECT_EQ(loaded.GetEnergyFieldState()->next_fire_tick(), 7u);
    runner.Shutdown();
}

TEST_F(WorldOpenRefusal, ServerFullSnapshotPresetChangeRefusesWithoutWritesOrAcknowledgement) {
    for (const bool active_regions : {false, true}) {
        SCOPED_TRACE(active_regions);
        for (const bool existing_snapshot : {false, true}) {
            SCOPED_TRACE(existing_snapshot);
            const auto config = SnapshotRunnerConfig(RootString(), active_regions);
            Server::ServerWorldRunner runner(config);
            BootSnapshotRunner(runner, active_regions);
            if (HasFatalFailure())
                return;
            auto* session = runner.Session();
            const auto destination = session->GetWorldSaveDir();
            const SnapshotPresetFiles preset_files(destination);

            std::shared_ptr<Chunk> chunk;
            PreparePendingSnapshot(*session, chunk, existing_snapshot);
            if (HasFatalFailure())
                return;
            const PendingSnapshotState pending(*session);

            for (const auto* changed : {"preset", "identity", "both"}) {
                SCOPED_TRACE(changed);
                preset_files.Rewrite(changed);
                if (HasFatalFailure())
                    return;
                const SnapshotDestinationBytes before(root);
                EXPECT_EQ(runner.SaveFullSnapshot(), 0u);
                before.ExpectUnchanged();
                pending.ExpectUnchanged(*session, *chunk);
                EXPECT_EQ(runner.StreamedChunkCount(), 1u);
                preset_files.Restore();
                if (HasFatalFailure())
                    return;
            }

            ASSERT_EQ(runner.SaveFullSnapshot(), 1u);
            EXPECT_FALSE(chunk->is_voxel_data_dirty());
            ExpectSnapshotReloaded(pending, *chunk, config, session->GetMetadata().worldId, jobs);
            if (HasFatalFailure())
                return;
            runner.Shutdown();
        }
    }
}

void WorldOpenRefusal::CheckLegacyFullSnapshotRefusalAndRecovery() {
    // This case exercises the legacy clock path, independently of shipping defaults.
    Write(root / "data/common/systems.json", R"({"sim":{"active_regions":{"enabled":false}}})");
    Server::ServerWorldRunnerConfig config;
    config.root_path = RootString();
    config.world_id = "fixture";
    config.surface_radius = 0;
    config.collision_radius = 0;
    Server::ServerWorldRunner runner(config);
    ASSERT_TRUE(runner.Boot()) << runner.GetBootError();
    ASSERT_FALSE(runner.Session()->ActiveRegionsEnabled());
    runner.Session()->GetWorldSystem()->clear_world(runner.Session()->GetPhysicsSystem());
    ASSERT_TRUE(runner.Session()->LoadWorldState());
    const auto chunks = runner.StreamedChunkCount();
    ASSERT_GT(chunks, 0u);
    bool called = false;
    std::size_t callback_count = 0;
    runner.Session()->GetSimulationEventBus().subscribe([&](const auto&) {
        called = true;
        ++callback_count;
        EXPECT_EQ(runner.SaveFullSnapshot(), chunks);
    });
    runner.Session()->GetSimulationEventBus().publish(1, "save", "");
    ASSERT_EQ(runner.Session()->TickSimulation(runner.Session()->GetSimulationClock().fixed_dt()),
              1u);
    EXPECT_TRUE(called);
    EXPECT_EQ(callback_count, 1u);
    const auto* world_system = runner.Session()->GetWorldSystem();
    const auto metadata = runner.Session()->GetMetadata();
    const auto streamed = world_system->snapshot_streamed_chunks();
    // A refused candidate leaves the current world saveable.
    const auto pending_chunks = runner.Session()->GetWorldSystem()->snapshot_streamed_chunks();
    ASSERT_FALSE(pending_chunks.empty());
    const auto& pending_chunk = pending_chunks.front();
    ASSERT_FALSE(pending_chunk->heightmap_data.empty());
    pending_chunk->heightmap_data.front() += 1.0f;
    pending_chunk->mark_voxel_data_dirty();
    ASSERT_FALSE(runner.Session()->LoadWorld("missing-world"));
    ASSERT_TRUE(runner.Session()->SaveWorld());
    ASSERT_EQ(runner.Session()->GetWorldSaveDir(), save);
    EXPECT_EQ(runner.SaveFullSnapshot(), chunks);
    const auto before_fault = DiskBytes(save);

    // A valid, empty tick-zero sidecar independently requires active-region metadata.
    // The legacy session must refuse that metadata save, while chunk persistence remains
    // valid. Keep a hard-link witness outside the save to verify ownership before removal.
    const auto ledger_path = WorldSaveService::active_regions_path(save);
    const auto ledger_owner = root / "metadata-failure-ledger";
    ASSERT_FALSE(fs::exists(ledger_path));
    ASSERT_FALSE(fs::exists(ledger_owner));
    const auto ledger_bytes = world::ActiveRegionLedger{}.encode();
    ASSERT_NO_FATAL_FAILURE(Write(ledger_path, ledger_bytes));
    fs::create_hard_link(ledger_path, ledger_owner);
    ASSERT_TRUE(fs::equivalent(ledger_path, ledger_owner));
    ASSERT_EQ(fs::hard_link_count(ledger_path), 2u);
    world::WorldClock stored_clock;
    bool requires_active_regions = false;
    ASSERT_TRUE(WorldSaveService::read_clock_metadata(save, stored_clock, requires_active_regions));
    ASSERT_TRUE(requires_active_regions);
    ASSERT_EQ(stored_clock.tick(), 0u);
    ASSERT_TRUE(WorldSaveService::validate_save(save));
    pending_chunk->mark_voxel_data_dirty();
    const auto fault_bytes = DiskBytes(save);
    ASSERT_FALSE(runner.Session()->SaveWorld());
    EXPECT_EQ(DiskBytes(save), fault_bytes);
    const SnapshotDestinationBytes before(root);
    EXPECT_EQ(runner.SaveFullSnapshot(), 0u);
    before.ExpectUnchanged();
    EXPECT_TRUE(pending_chunk->is_voxel_data_dirty());
    EXPECT_EQ(runner.StreamedChunkCount(), chunks);
    EXPECT_EQ(DiskBytes(save), fault_bytes);

    ASSERT_TRUE(fs::equivalent(ledger_path, ledger_owner));
    ASSERT_EQ(Read(ledger_path), ledger_bytes);
    ASSERT_EQ(Read(ledger_owner), ledger_bytes);
    ASSERT_EQ(fs::hard_link_count(ledger_path), 2u);
    ASSERT_TRUE(fs::remove(ledger_path));
    ASSERT_EQ(fs::hard_link_count(ledger_owner), 1u);
    ASSERT_TRUE(fs::remove(ledger_owner));
    EXPECT_EQ(DiskBytes(save), before_fault);
    ASSERT_TRUE(runner.Session()->SaveWorld());
    EXPECT_EQ(DiskBytes(save), before_fault);
    EXPECT_EQ(runner.Session()->GetWorldSystem(), world_system);
    EXPECT_EQ(world_system->snapshot_streamed_chunks(), streamed);
    EXPECT_EQ(runner.StreamedChunkCount(), chunks);
    const auto& retained = runner.Session()->GetMetadata();
    EXPECT_EQ(retained.worldId, metadata.worldId);
    EXPECT_EQ(retained.name, metadata.name);
    EXPECT_EQ(retained.seed, metadata.seed);
    EXPECT_EQ(retained.worldType, metadata.worldType);
    EXPECT_EQ(retained.creationTime, metadata.creationTime);
    EXPECT_EQ(retained.spawnPoint, metadata.spawnPoint);
    EXPECT_EQ(callback_count, 1u);
    runner.Shutdown();
}

TEST_F(WorldOpenRefusal, LegacyFullSnapshotCallbackAndMetadataFailureKeepChunkCount) {
    CheckLegacyFullSnapshotRefusalAndRecovery();
}

TEST_F(WorldOpenRefusal, LegacyFullSnapshotCallbackSucceedsAndMetadataFailureRefuses) {
    CheckLegacyFullSnapshotRefusalAndRecovery();
}

TEST_F(WorldOpenRefusal, LegacyExplicitSessionSnapshotsKeepLargeKeylessMetadata) {
    auto metadata = nlohmann::json::parse(Read(save / "world_info.json"));
    metadata["extension"] = std::string(std::size_t{1024} * 1024, 'x');
    Write(save / "world_info.json", metadata.dump());
    GameSession session;
    session.SetRootPath(RootString());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(false); // Legacy keyless snapshots have no clock metadata.
    ASSERT_TRUE(session.CreateTransientWorld("Legacy direct load", "1337", "default"));
    ASSERT_TRUE(session.LoadWorldStateFrom(save)) << session.GetWorldOpenError();
    EXPECT_TRUE(session.SaveWorldStateTo(save));
    // Metadata reconciliation retains extensions and persists the legacy water cursor.
    metadata["waterSimCursor"] = 0;
    EXPECT_EQ(Read(save / "world_info.json"), metadata.dump(4) + "\n");
}

TEST_F(WorldOpenRefusal, LegacyExplicitSessionSnapshotsKeepLargeExtensions) {
    auto metadata = nlohmann::json::parse(Read(save / "world_info.json"));
    metadata["extension"] = std::string(std::size_t{1024} * 1024, 'x');
    Write(save / "world_info.json", metadata.dump());
    GameSession session;
    session.SetRootPath(RootString());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(false); // Legacy keyless snapshots have no clock metadata.
    ASSERT_TRUE(session.CreateTransientWorld("Legacy direct load", "1337", "default"));
    ASSERT_TRUE(session.LoadWorldStateFrom(save)) << session.GetWorldOpenError();
    for (const bool full_snapshot : {false, true}) {
        EXPECT_TRUE(session.SaveWorldStateTo(save, nullptr, full_snapshot));
        auto expected = metadata;
        expected["waterSimCursor"] = 0;
        EXPECT_EQ(Read(save / "world_info.json"), expected.dump(4) + "\n");
    }
}

TEST_F(WorldOpenRefusal, DirectoryWatchesRetainBothFiltersAndDiscoverNestedFiles) {
    using Luminumbra::Client::UI::UIHotReload;
    const auto ui = root / "ui";
    fs::create_directories(ui / "nested");
    UIHotReload reload;
    reload.SetEnabled(true);
    reload.WatchDirectory(ui, "rml");
    reload.WatchDirectory(ui, ".rcss");
    reload.WatchDirectory(ui, "rml"); // duplicate registration is harmless
    std::vector<std::string> changes;
    reload.SetReloadCallback([&](const std::string& path) { changes.push_back(path); });
    const auto parent_time = fs::last_write_time(ui);
    Write(ui / "nested/new.rml", "document");
    Write(ui / "nested/new.rcss", "style");
    Write(ui / "nested/ignored.txt", "ignored");
    // Nested file creation changes nested/, not its parent. Observe that property
    // directly: setting directory timestamps requires different rights on Windows.
    ASSERT_EQ(fs::last_write_time(ui), parent_time);
    reload.Update();
    EXPECT_TRUE(changes.empty()); // Polling remains throttled before the interval expires.
    reload.SetEnabled(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(1010));
    reload.Update();
    EXPECT_TRUE(changes.empty());
    reload.SetEnabled(true);
    reload.Update();
    std::sort(changes.begin(), changes.end());
    const std::array<fs::path, 2> documents{ui / "nested/new.rcss", ui / "nested/new.rml"};
    const std::vector<std::string> expected = {Filesystem::GenericUtf8PathString(documents[0]),
                                               Filesystem::GenericUtf8PathString(documents[1])};
    EXPECT_EQ(changes, expected);
    EXPECT_EQ(fs::last_write_time(ui), parent_time);
    changes.clear();
    for (const auto& path : documents)
        fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(2));
    std::this_thread::sleep_for(std::chrono::milliseconds(1010));
    reload.Update();
    std::sort(changes.begin(), changes.end());
    EXPECT_EQ(changes, expected);
    reload.StopWatching(ui);
    changes.clear();
    Write(ui / "nested/later.rml", "later");
    std::this_thread::sleep_for(std::chrono::milliseconds(1010));
    reload.Update();
    EXPECT_TRUE(changes.empty());
    reload.ClearAllWatches();
    for (const auto& path : documents)
        fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(2));
    std::this_thread::sleep_for(std::chrono::milliseconds(1010));
    reload.Update();
    EXPECT_TRUE(changes.empty());
}

TEST_F(WorldOpenRefusal, DirectoryDiscoveryRehashesWatchTableAndPreservesOrdinaryChanges) {
    using Luminumbra::Client::UI::UIHotReload;
    const auto ui = root / "ui";
    const auto original = ui / "original.rml";
    ASSERT_NO_FATAL_FAILURE(Write(original, "original"));
    UIHotReload reload;
    reload.SetEnabled(true);
    reload.WatchDirectory(ui, "rml");
    const auto buckets_before = reload.WatchedFileBucketCount();
    // Bound the fixture to 256 additions, exceeding the initial table's capacity.
    constexpr std::size_t additions = 256;
    ASSERT_LT(buckets_before, additions);
    std::vector<std::string> expected{Filesystem::GenericUtf8PathString(original)};
    for (std::size_t index = 0; index < additions; ++index) {
        const auto path = ui / "nested" / (std::to_string(index) + ".rml");
        ASSERT_NO_FATAL_FAILURE(Write(path, "new document"));
        expected.push_back(Filesystem::GenericUtf8PathString(path));
    }
    ASSERT_NO_FATAL_FAILURE(Write(ui / "ignored.txt", "ignored"));
    fs::last_write_time(original, fs::last_write_time(original) + std::chrono::seconds(2));
    std::vector<std::string> changes;
    reload.SetReloadCallback([&](const std::string& path) { changes.push_back(path); });
    std::this_thread::sleep_for(std::chrono::milliseconds(1010));
    reload.Update();
    EXPECT_GT(reload.WatchedFileBucketCount(), buckets_before);
    std::sort(expected.begin(), expected.end());
    std::sort(changes.begin(), changes.end());
    EXPECT_EQ(changes, expected);
    changes.clear();
    std::this_thread::sleep_for(std::chrono::milliseconds(1010));
    reload.Update();
    EXPECT_TRUE(changes.empty());
}

TEST_F(WorldOpenRefusal, ReloadCallbackAddsFileAndDirectoryWatchesForLaterPolls) {
    using Luminumbra::Client::UI::UIHotReload;
    const auto original = root / "original.rml";
    const auto added_file = root / "added.rml";
    const auto added_directory = root / "added";
    const auto added_document = added_directory / "existing.rml";
    for (const auto& path : {original, added_file, added_document}) {
        ASSERT_NO_FATAL_FAILURE(Write(path, "initial"));
    }
    UIHotReload reload;
    reload.SetEnabled(true);
    reload.WatchFile(original);
    std::vector<std::string> changes;
    bool watches_added = false;
    reload.SetReloadCallback([&](const std::string& path) {
        changes.push_back(path);
        if (!watches_added) {
            watches_added = true;
            reload.WatchFile(added_file);
            reload.WatchDirectory(added_directory, "rml");
        }
    });
    fs::last_write_time(original, fs::last_write_time(original) + std::chrono::seconds(2));
    std::this_thread::sleep_for(std::chrono::milliseconds(1010));
    reload.Update();
    EXPECT_EQ(changes, std::vector<std::string>{Filesystem::GenericUtf8PathString(original)});
    changes.clear();
    const auto discovered = added_directory / "nested/new.rml";
    ASSERT_NO_FATAL_FAILURE(Write(discovered, "discovered"));
    for (const auto& path : {added_file, added_document}) {
        fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(2));
    }
    std::vector<std::string> expected{Filesystem::GenericUtf8PathString(added_file),
                                      Filesystem::GenericUtf8PathString(added_document),
                                      Filesystem::GenericUtf8PathString(discovered)};
    std::this_thread::sleep_for(std::chrono::milliseconds(1010));
    reload.Update();
    std::sort(expected.begin(), expected.end());
    std::sort(changes.begin(), changes.end());
    EXPECT_EQ(changes, expected);
}

TEST_F(WorldOpenRefusal, ReloadCallbackRemovesOwnAndOtherWatchesAfterDiscovery) {
    using Luminumbra::Client::UI::UIHotReload;
    const std::array<fs::path, 2> documents{root / "first.rml", root / "second.rml"};
    UIHotReload reload;
    reload.SetEnabled(true);
    for (const auto& path : documents) {
        ASSERT_NO_FATAL_FAILURE(Write(path, "initial"));
        reload.WatchFile(path);
        fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(2));
    }
    std::vector<std::string> changes;
    reload.SetReloadCallback([&](const std::string& path) {
        changes.push_back(path);
        // Remove both the executing watch and the other queued watch in either order.
        for (const auto& document : documents) {
            reload.StopWatching(document);
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(1010));
    reload.Update();
    std::sort(changes.begin(), changes.end());
    const std::vector<std::string> expected{Filesystem::GenericUtf8PathString(documents[0]),
                                            Filesystem::GenericUtf8PathString(documents[1])};
    EXPECT_EQ(changes, expected);
    changes.clear();
    for (const auto& path : documents) {
        fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(2));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1010));
    reload.Update();
    EXPECT_TRUE(changes.empty());
}

TEST_F(WorldOpenRefusal, ReloadCallbackCanDisableOrClearWatchesWhileQueuedChangesDrain) {
    using Luminumbra::Client::UI::UIHotReload;
    const auto ui = root / "ui";
    const std::array<fs::path, 2> documents{ui / "first.rml", ui / "second.rml"};
    for (const bool clear_watches : {false, true}) {
        SCOPED_TRACE(clear_watches);
        for (const auto& path : documents) {
            ASSERT_NO_FATAL_FAILURE(Write(path, "initial"));
        }
        UIHotReload reload;
        reload.SetEnabled(true);
        reload.WatchDirectory(ui, "rml");
        std::vector<std::string> changes;
        reload.SetReloadCallback([&](const std::string& path) {
            changes.push_back(path);
            if (clear_watches) {
                reload.ClearAllWatches();
            } else {
                reload.SetEnabled(false);
            }
        });
        for (const auto& path : documents) {
            fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(2));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1010));
        reload.Update();
        std::sort(changes.begin(), changes.end());
        const std::vector<std::string> expected{Filesystem::GenericUtf8PathString(documents[0]),
                                                Filesystem::GenericUtf8PathString(documents[1])};
        EXPECT_EQ(changes, expected);
        changes.clear();
        for (const auto& path : documents) {
            fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(2));
        }
        ASSERT_NO_FATAL_FAILURE(Write(ui / "new.rml", "new document"));
        std::this_thread::sleep_for(std::chrono::milliseconds(1010));
        reload.Update();
        EXPECT_TRUE(changes.empty());
        reload.SetEnabled(true);
        reload.Update();
        auto resumed_expected = clear_watches ? std::vector<std::string>{} : expected;
        if (!clear_watches) {
            resumed_expected.push_back(Filesystem::GenericUtf8PathString(ui / "new.rml"));
        }
        std::sort(resumed_expected.begin(), resumed_expected.end());
        std::sort(changes.begin(), changes.end());
        EXPECT_EQ(changes, resumed_expected);
        fs::remove(ui / "new.rml");
    }
}

TEST_F(WorldOpenRefusal, ReloadCallbackLifetimeSurvivesClearingAndReplacementOnEveryEntryPoint) {
    using Luminumbra::Client::UI::UIHotReload;
    const auto document = root / "document.rml";
    ASSERT_NO_FATAL_FAILURE(Write(document, "initial"));
    for (const bool replace_callback : {false, true}) {
        for (const int entry_point : {0, 1, 2}) {
            SCOPED_TRACE(replace_callback);
            SCOPED_TRACE(entry_point);
            UIHotReload reload;
            reload.SetEnabled(true);
            reload.WatchFile(document);
            unsigned calls = 0;
            unsigned replacement_calls = 0;
            auto lifetime = std::make_shared<const int>(42);
            const std::weak_ptr<const int> registered_lifetime = lifetime;
            reload.SetReloadCallback([&, lifetime](const std::string& path) {
                const std::weak_ptr<const int> executing_lifetime = lifetime;
                // Retain all state used after registration changes outside the callable.
                auto* const call_count = &calls;
                EXPECT_EQ(path,
                          entry_point == 1 ? std::string{}
                                           : Filesystem::GenericUtf8PathString(document));
                if (replace_callback) {
                    reload.SetReloadCallback([&](const std::string&) { ++replacement_calls; });
                } else {
                    reload.SetReloadCallback({});
                }
                EXPECT_FALSE(executing_lifetime.expired());
                ++*call_count;
            });
            lifetime.reset();
            ASSERT_FALSE(registered_lifetime.expired());
            if (entry_point == 0) {
                reload.TriggerReload(document);
            } else if (entry_point == 1) {
                reload.ReloadAll();
            } else {
                fs::last_write_time(document,
                                    fs::last_write_time(document) + std::chrono::seconds(2));
                std::this_thread::sleep_for(std::chrono::milliseconds(1010));
                reload.Update();
            }
            EXPECT_EQ(calls, 1u);
            EXPECT_TRUE(registered_lifetime.expired());
            EXPECT_EQ(replacement_calls, 0u);
            reload.TriggerReload(document);
            reload.ReloadAll();
            EXPECT_EQ(calls, 1u);
            EXPECT_EQ(replacement_calls, replace_callback ? 2u : 0u);
        }
    }
}

TEST_F(WorldOpenRefusal, ReloadCallbackRetainsMutableStateAndSupportsNestedDelivery) {
    using Luminumbra::Client::UI::UIHotReload;
    UIHotReload reload;
    std::vector<unsigned> calls;
    reload.SetReloadCallback([&, count = 0u](const std::string&) mutable {
        calls.push_back(++count);
        if (count == 1u) {
            reload.ReloadAll();
        }
    });
    reload.TriggerReload(root / "manual.rml");
    reload.ReloadAll();
    EXPECT_EQ(calls, (std::vector<unsigned>{1u, 2u, 3u}));
}

TEST_F(WorldOpenRefusal, DirectoryWatchWildcardAndMissingDirectoryRemainSafe) {
    using Luminumbra::Client::UI::UIHotReload;
    const auto ui = root / "ui";
    UIHotReload reload;
    reload.SetEnabled(true);
    EXPECT_NO_THROW(reload.WatchDirectory(ui, "rml"));
    fs::create_directory(ui);
    reload.WatchDirectory(ui);
    reload.WatchDirectory(ui, "rcss");
    std::vector<std::string> changes;
    reload.SetReloadCallback([&](const std::string& path) { changes.push_back(path); });
    Write(ui / "new.txt", "wildcard");
    std::this_thread::sleep_for(std::chrono::milliseconds(1010));
    reload.Update();
    EXPECT_EQ(changes, std::vector<std::string>{Filesystem::GenericUtf8PathString(ui / "new.txt")});
    fs::remove_all(ui);
    std::this_thread::sleep_for(std::chrono::milliseconds(1010));
    EXPECT_NO_THROW(reload.Update());
}

TEST_F(WorldOpenRefusal, AllEnergyOpenPathsRefuseWithoutAdoptingOrWriting) {
    GameSession session;
    session.SetRootPath(RootString());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(false);
    session.SetAetherStateEnabled(true);
    ASSERT_TRUE(session.CreateTransientWorld("Live world", "1337", "default"));
    ASSERT_TRUE(session.LoadWorldStateFrom(save));
    const auto loaded_count = session.GetLastLoadedChunkCount();
    ASSERT_EQ(loaded_count, 1u);
    session.GetWorldSystem()->clear_world(session.GetPhysicsSystem());
    std::shared_ptr<Chunk> chunk;
    ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(session, chunk, false));
    const PendingSnapshotState pending(session);
    std::string too_many_pages = "EFS1 2 8\n";
    for (std::size_t page = 0; page <= luminumbra::fields::kEnergyReadPages; ++page)
        too_many_pages += "P " + std::to_string(page) + " 0 0:1\n";
    for (const auto& invalid :
         {std::string("EFS1 17 8\n"), too_many_pages, std::string("EFS1 2 8\nP 0 0 0:1junk\n")}) {
        Write(save / "aether_state.efs", invalid);
        const auto before = DiskBytes(root);
        EXPECT_FALSE(session.LoadWorld("fixture"));
        pending.ExpectUnchanged(session, *chunk);
        EXPECT_EQ(session.GetLastLoadedChunkCount(), loaded_count);
        EXPECT_FALSE(session.LoadWorldFrom("fixture", root));
        pending.ExpectUnchanged(session, *chunk);
        EXPECT_EQ(session.GetLastLoadedChunkCount(), loaded_count);
        EXPECT_FALSE(session.LoadWorldStateFrom(save));
        pending.ExpectUnchanged(session, *chunk);
        EXPECT_EQ(session.GetLastLoadedChunkCount(), loaded_count);
        EXPECT_EQ(DiskBytes(root), before);
    }
    Write(save / "aether_state.efs", "EFS1 16 8\nP 0 0 4095:65535\n");
    EXPECT_TRUE(session.LoadWorldStateFrom(save)) << session.GetWorldOpenError();
    EXPECT_EQ(session.GetEnergyFieldState()->channels(), 16);
}

TEST_F(WorldOpenRefusal, OversizedEnergyFileIsRefusedBeforeBufferingAndPartialHeaderIsInvalid) {
    const auto path = save / "aether_state.efs";
    Write(path, "EFS1 2 8\n");
    fs::resize_file(path, luminumbra::fields::kEnergyReadRecordBytes + 1);
    {
        const Persistence::SavedInputFile input(path);
        std::string record = "unchanged";
        EXPECT_FALSE(Persistence::ReadSavedEnergy(input, record));
        EXPECT_EQ(record, "unchanged");
    }
    for (const auto* malformed : {"EFS1 2", "EFS1 2 8\nP 0 0 0:1junk\n"}) {
        Write(path, malformed);
        const Persistence::SavedInputFile partial(path);
        std::string record = "unchanged";
        EXPECT_FALSE(Persistence::ReadSavedEnergy(partial, record));
        EXPECT_EQ(record, "unchanged");
    }
}

TEST_F(WorldOpenRefusal, AdmissionWordsInPathsDoNotReclassifyCorruptRegionRefusals) {
    const auto snapshot = root / "oversized compatibility limit";
    fs::copy(save, snapshot, fs::copy_options::recursive);
    Write(WorldSaveService::region_file_path(snapshot, 0, 0), std::string("BAD!\2\0\0\0", 8));
    GameSession session;
    session.SetRootPath(RootString());
    session.SetJobSystem(&jobs);
    session.SetAetherStateEnabled(true);
    ASSERT_TRUE(session.CreateTransientWorld("Live world", "1337", "default"));
    session.GetWorldSystem()->clear_world(session.GetPhysicsSystem());
    std::shared_ptr<Chunk> chunk;
    ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(session, chunk, false));
    const auto before = DiskBytes(root);
    EXPECT_FALSE(session.LoadWorldStateFrom(snapshot));
    EXPECT_TRUE(session.GetWorldSystem()->snapshot_streamed_chunks().empty());
    EXPECT_FALSE(session.SaveWorldStateTo(save));
    EXPECT_EQ(DiskBytes(root), before);
}

TEST_F(WorldOpenRefusal, EnergyWriterCapRefusesBeforeAnySiblingWriterOrDirtyAcknowledgement) {
    const auto config = SnapshotRunnerConfig(RootString(), false);
    Server::ServerWorldRunner runner(config);
    ASSERT_NO_FATAL_FAILURE(BootSnapshotRunner(runner, false));
    auto* session = runner.Session();
    std::shared_ptr<Chunk> chunk;
    ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(*session, chunk, true));
    std::string record = "EFS1 2 8\n";
    for (std::size_t page = 0; page <= luminumbra::fields::kEnergyWritePages; ++page)
        record += "P " + std::to_string(page) + " 0 0:1\n";
    ASSERT_TRUE(session->GetEnergyFieldState()->DeserializeRecord(record, 0));
    const PendingSnapshotState pending(*session);
    const auto before = DiskBytes(root);
    for (const bool full : {false, true}) {
        EXPECT_FALSE(session->SaveWorldStateTo(session->GetWorldSaveDir(), nullptr, full));
        pending.ExpectUnchanged(*session, *chunk);
        EXPECT_EQ(DiskBytes(root), before);
    }
}

TEST_F(WorldOpenRefusal, CleanOversizedRegionDoesNotBlockIndependentPayloadSaves) {
    for (const bool active_regions : {false, true}) {
        const auto config = SnapshotRunnerConfig(RootString(), active_regions);
        Server::ServerWorldRunner runner(config);
        ASSERT_NO_FATAL_FAILURE(BootSnapshotRunner(runner, active_regions));
        auto* session = runner.Session();
        std::shared_ptr<Chunk> chunk;
        ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(*session, chunk, true));
        chunk->clear_voxel_data_dirty();
        // The compact array encoding stores each float in four bytes, and base64 grows that by a
        // third, so a quarter of the cap in floats is enough for this one resident chunk to
        // exceed the 64 MiB record cap.
        chunk->heightmap_data.assign(Persistence::SavedInputLimits::kRegionRecordBytes / 4 + 1,
                                     std::numeric_limits<float>::max());
        const auto region = WorldSaveService::region_file_path(session->GetWorldSaveDir(), 0, 0);
        const auto region_before = Read(region);
        const auto manifest = WorldSaveService::world_manifest_path(session->GetWorldSaveDir());
        const auto manifest_before = Read(manifest);
        const auto plant_path = WorldSaveService::plant_entities_path(session->GetWorldSaveDir());
        const auto energy_path = session->GetWorldSaveDir() / "aether_state.efs";
        const auto plants_before = Read(plant_path);
        const auto energy_before = Read(energy_path);
        // Restore energy so only the plant payload changes in this save.
        auto* energy_state = session->GetEnergyFieldState();
        ASSERT_TRUE(active_regions ? energy_state->DeserializeRegionalRecord(energy_before)
                                   : energy_state->DeserializeRecord(
                                         energy_before, session->GetSimulationTickCount()));
        ASSERT_TRUE(session->SaveWorldState());
        EXPECT_NE(Read(plant_path), plants_before);
        EXPECT_EQ(Read(energy_path), energy_before);
        const auto plants_after = Read(plant_path);
        session->GetEnergyFieldState()->QueueDeposit(2, 0, 0, 0, 1000);
        ASSERT_NO_FATAL_FAILURE(
            ApplySnapshotTestEnergy(*session, session->GetSimulationTickCount()));
        ASSERT_TRUE(session->SaveWorldState());
        EXPECT_EQ(Read(plant_path), plants_after);
        EXPECT_NE(Read(energy_path), energy_before);
        EXPECT_EQ(Read(region), region_before);
        EXPECT_EQ(Read(manifest), manifest_before);
        EXPECT_FALSE(chunk->is_voxel_data_dirty());
        // A dirty region elsewhere does not pull the oversized clean region
        // into the incremental write set.
        auto other = std::make_shared<Chunk>(IVec3(32, 0, 0));
        other->set_state(ChunkState::Idle);
        other->heightmap_data = {8.0f};
        other->mark_voxel_data_dirty();
        ASSERT_TRUE(session->GetWorldSystem()->adopt_streamed_chunk(other));
        ASSERT_TRUE(session->SaveWorldState());
        EXPECT_EQ(Read(region), region_before);
        EXPECT_FALSE(other->is_voxel_data_dirty());
        const auto before = DiskBytes(root);
        chunk->mark_voxel_data_dirty();
        EXPECT_FALSE(session->SaveWorldState());
        EXPECT_TRUE(chunk->is_voxel_data_dirty());
        EXPECT_EQ(DiskBytes(root), before);
        // A clean chunk in the SAME region must still be admitted when that
        // region is replaced, and full snapshots must admit all residents.
        chunk->clear_voxel_data_dirty();
        auto neighbour = std::make_shared<Chunk>(IVec3(1, 0, 0));
        neighbour->set_state(ChunkState::Idle);
        neighbour->mark_voxel_data_dirty();
        ASSERT_TRUE(session->GetWorldSystem()->adopt_streamed_chunk(neighbour));
        EXPECT_FALSE(session->SaveWorldState());
        EXPECT_TRUE(neighbour->is_voxel_data_dirty());
        EXPECT_EQ(DiskBytes(root), before);
        EXPECT_FALSE(session->SaveWorldStateTo(session->GetWorldSaveDir(), nullptr, true));
        EXPECT_EQ(DiskBytes(root), before);
        // Release the large fixture before shutdown, which autosaves.
        chunk->heightmap_data = {42.0f};
        runner.Shutdown();
    }
}

TEST_F(WorldOpenRefusal, EmbeddedPresetDepthBoundaryPrecedesCatalogAndSessionParsing) {
    GameSession session;
    session.SetRootPath(RootString());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(session.CreateWorld("Preset depth", "1337", "default"));
    const auto destination = session.GetWorldSaveDir();
    const auto id = session.GetMetadata().worldId;
    const auto preset_path = destination / "preset.json";
    auto preset = nlohmann::json::parse(Read(preset_path));
    nlohmann::json extension = 0;
    for (std::size_t depth = 1; depth < Persistence::SavedInputLimits::kJsonNestingDepth; ++depth)
        extension = nlohmann::json::array({std::move(extension)});
    preset["extension"] = extension;
    auto metadata = nlohmann::json::parse(Read(destination / "world_info.json"));
    metadata["presetIdentity"] = Persistence::StableChecksum(preset.dump());
    Write(preset_path, preset.dump());
    Write(destination / "world_info.json", metadata.dump());
    EXPECT_TRUE(Persistence::InspectSavedWorld(root, id).error.empty());
    preset["extension"] = nlohmann::json::array({std::move(extension)});
    Write(preset_path, preset.dump());
    const auto before = DiskBytes(root);
    EXPECT_FALSE(Persistence::InspectSavedWorld(root, id).error.empty());
    EXPECT_FALSE(session.LoadWorld(id));
    EXPECT_FALSE(session.SaveWorldState());
    EXPECT_EQ(DiskBytes(root), before);
}

TEST_F(WorldOpenRefusal, EmbeddedPresetByteLimitIsInclusiveAndRefusesWithoutWrites) {
    GameSession session;
    session.SetRootPath(RootString());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(session.CreateWorld("Preset bytes", "1337", "default"));
    const auto destination = session.GetWorldSaveDir();
    const auto id = session.GetMetadata().worldId;
    const auto preset_path = destination / "preset.json";
    auto bytes = Read(preset_path);
    bytes.resize(Persistence::SavedInputLimits::kPresetBytes, ' ');
    Write(preset_path, bytes);
    ASSERT_TRUE(Persistence::InspectSavedWorld(root, id).error.empty());
    ASSERT_TRUE(session.SaveWorldState());
    EXPECT_EQ(Read(preset_path), bytes);
    bytes += ' ';
    Write(preset_path, bytes);
    const auto before = DiskBytes(root);
    EXPECT_FALSE(Persistence::InspectSavedWorld(root, id).error.empty());
    EXPECT_FALSE(session.LoadWorld(id));
    EXPECT_FALSE(session.SaveWorldState());
    EXPECT_EQ(DiskBytes(root), before);
}

TEST_F(WorldOpenRefusal, NewWorldPresetIdentityReopensAtTheCompactWriteBoundary) {
    GameSession session;
    session.SetRootPath(RootString());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(session.CreateWorld("Canonical preset", "1337", "default"));
    auto preset = nlohmann::json::parse(Read(session.GetWorldSaveDir() / "preset.json"));
    for (const bool near_limit : {false, true}) {
        SCOPED_TRACE(near_limit);
        preset["extension"] = "";
        if (near_limit)
            preset["extension"] = std::string(
                Persistence::SavedInputLimits::kPresetBytes - 1 - preset.dump().size(), 'x');
        const auto canonical = preset.dump();
        ASSERT_TRUE(session.CreateWorld("Reopen preset", "1337", "default", &canonical));
        const auto destination = session.GetWorldSaveDir();
        const auto id = session.GetMetadata().worldId;
        EXPECT_EQ(Read(destination / "preset.json"), canonical + "\n");
        const auto metadata = nlohmann::json::parse(Read(destination / "world_info.json"));
        EXPECT_EQ(metadata.at("presetIdentity"), Persistence::StableChecksum(canonical));
        const auto inspected = Persistence::InspectSavedWorld(root, id);
        ASSERT_TRUE(inspected.error.empty()) << inspected.error;
        EXPECT_EQ(inspected.preset_json, canonical);
        ASSERT_TRUE(session.LoadWorld(id)) << session.GetWorldOpenError();
    }
}

TEST_F(WorldOpenRefusal, ExactCompactPresetLoadsAndSavesWithoutReplacementButRefusesFreshExport) {
    GameSession session;
    session.SetRootPath(RootString());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(session.CreateWorld("Full preset", "1337", "default"));
    const auto destination = session.GetWorldSaveDir();
    const auto id = session.GetMetadata().worldId;
    auto preset = nlohmann::json::parse(Read(destination / "preset.json"));
    preset["extension"] = "";
    preset["extension"] =
        std::string(Persistence::SavedInputLimits::kPresetBytes - preset.dump().size(), 'x');
    const auto canonical = preset.dump();
    ASSERT_EQ(canonical.size(), Persistence::SavedInputLimits::kPresetBytes);
    Write(destination / "preset.json", canonical);
    auto metadata = nlohmann::json::parse(Read(destination / "world_info.json"));
    metadata["presetIdentity"] = Persistence::StableChecksum(canonical);
    Write(destination / "world_info.json", metadata.dump());
    ASSERT_TRUE(session.LoadWorld(id)) << session.GetWorldOpenError();
    ASSERT_TRUE(session.SaveWorldState());
    ASSERT_TRUE(session.SaveWorld());
    EXPECT_EQ(Read(destination / "preset.json"), canonical);
    const auto before = DiskBytes(root);
    EXPECT_FALSE(session.SaveWorldStateTo(root / "full-preset-export", nullptr, true));
    EXPECT_EQ(DiskBytes(root), before);

    // An admitted file can expand on compact dumping (1e2 becomes 100.0).
    // Reusing that existing file must not apply a new-file encoding budget.
    preset["extension"] = nullptr;
    auto encoded = preset.dump();
    const auto offset = encoded.find("\"extension\":null");
    ASSERT_NE(offset, std::string::npos);
    const auto count = Persistence::SavedInputLimits::kPresetBytes / 5;
    std::string numbers = "[";
    numbers.reserve(count * 4 + 1);
    for (std::size_t index = 0; index < count; ++index)
        numbers += index + 1 == count ? "1e2]" : "1e2,";
    encoded.replace(offset + std::string("\"extension\":").size(), 4, numbers);
    ASSERT_LT(encoded.size(), Persistence::SavedInputLimits::kPresetBytes);
    const auto expanded_canonical = nlohmann::json::parse(encoded).dump();
    ASSERT_GT(expanded_canonical.size(), Persistence::SavedInputLimits::kPresetBytes);
    Write(destination / "preset.json", encoded);
    metadata["presetIdentity"] = Persistence::StableChecksum(expanded_canonical);
    Write(destination / "world_info.json", metadata.dump());
    ASSERT_TRUE(session.LoadWorld(id)) << session.GetWorldOpenError();
    ASSERT_TRUE(session.SaveWorld());
    EXPECT_EQ(Read(destination / "preset.json"), encoded);
}

TEST_F(WorldOpenRefusal, PresetAdmissionRefusesCreationBeforeDirectoryOrLiveStateChanges) {
    GameSession session;
    session.SetRootPath(RootString());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(session.CreateWorld("Existing world", "1337", "default"));
    const auto* previous_system = session.GetWorldSystem();
    const auto previous_id = session.GetMetadata().worldId;
    const auto preset = nlohmann::json::parse(Read(session.GetWorldSaveDir() / "preset.json"));
    nlohmann::json extension = 0;
    for (std::size_t depth = 0; depth < Persistence::SavedInputLimits::kJsonNestingDepth; ++depth)
        extension = nlohmann::json::array({std::move(extension)});
    auto deep = preset;
    deep["extension"] = std::move(extension);
    auto large = preset;
    large["extension"] = std::string(Persistence::SavedInputLimits::kPresetBytes, 'x');
    for (const auto& bytes : {deep.dump(), large.dump(), std::string("{")}) {
        const auto before = DiskBytes(root);
        EXPECT_FALSE(session.CreateWorld("Refused", "1337", "default", &bytes));
        EXPECT_EQ(session.GetWorldSystem(), previous_system);
        EXPECT_EQ(session.GetMetadata().worldId, previous_id);
        EXPECT_EQ(DiskBytes(root), before);
    }
}

TEST_F(WorldOpenRefusal, CatalogSharesWriterMetadataLimitAndTextLimitsAreInclusive) {
    const auto original = nlohmann::json::parse(Read(save / "world_info.json"));
    auto metadata = original;
    metadata["name"] = std::string(Persistence::SavedInputLimits::kWorldNameBytes, 'x');
    metadata["seed"] = std::string(Persistence::SavedInputLimits::kWorldSeedBytes, '7');
    auto bytes = metadata.dump();
    bytes.resize(Persistence::SavedInputLimits::kMetadataBytes, ' ');
    ASSERT_TRUE(WorldSaveService::save_metadata(bytes, save));
    EXPECT_TRUE(Persistence::InspectSavedWorld(root, "fixture").error.empty());
    bytes += ' ';
    Write(save / "world_info.json", bytes);
    EXPECT_FALSE(Persistence::InspectSavedWorld(root, "fixture").error.empty());
    for (const auto* key : {"name", "seed"}) {
        metadata = original;
        metadata[key] = std::string(Persistence::SavedInputLimits::kWorldNameBytes + 1, 'x');
        Write(save / "world_info.json", metadata.dump());
        EXPECT_FALSE(Persistence::InspectSavedWorld(root, "fixture").error.empty());
    }
}

TEST_F(WorldOpenRefusal, MetadataTextWriterRefusesBeforeWorldCreationChangesAnything) {
    GameSession session;
    session.SetRootPath(RootString());
    session.SetJobSystem(&jobs);
    ASSERT_TRUE(session.CreateTransientWorld("Live world", "1337", "default"));
    const auto before = DiskBytes(root);
    const auto old_name = session.GetMetadata().name;
    const std::string excessive(Persistence::SavedInputLimits::kWorldNameBytes + 1, 'x');
    EXPECT_FALSE(session.CreateWorld(excessive, "1337", "default"));
    EXPECT_FALSE(session.CreateWorld("Valid", excessive, "default"));
    EXPECT_EQ(session.GetMetadata().name, old_name);
    EXPECT_EQ(DiskBytes(root), before);
}

} // namespace

namespace {
using WeatherScript = Luminumbra::Systems::WeatherSchedule;
using WeatherKind = Luminumbra::Systems::WeatherCategory;
namespace P = Luminumbra::Persistence;

WeatherScript ShortWeatherScript() {
    return {.phases = {{0, WeatherKind::Clear, 0, 0},
                       {2, WeatherKind::Clear, 0, 0},
                       {5, WeatherKind::Rain, 65535, 65535},
                       {8, WeatherKind::Rain, 65535, 65535},
                       {11, WeatherKind::Clear, 0, 0}},
            .strike_tick = 7};
}

TEST_F(WorldOpenRefusal, OwnerContinuationTransientSnapshotRetainsWeatherScheduleAndExtensions) {
    const auto script = ShortWeatherScript();
    GameSession source({.owner_continuation = true});
    source.SetJobSystem(&jobs);
    source.SetRootPath(RootString());
    source.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(source.SetWeatherSchedule(script));
    ASSERT_TRUE(source.CreateTransientWorld("Scheduled owner snapshot", "1337", "default"));
    for (int tick = 0; tick < 3; ++tick) {
        ASSERT_EQ(source.TickSimulation(source.GetSimulationClock().fixed_dt()), 1u);
    }
    auto original = nlohmann::json::parse(Read(save / "world_info.json"));
    original["extension"] = {"retained"};
    Write(save / "world_info.json", original.dump());
    ASSERT_TRUE(source.SaveWorldStateTo(save, nullptr, true));
    const auto written = nlohmann::json::parse(Read(save / "world_info.json"));
    EXPECT_EQ(written.at("name"), original.at("name"));
    EXPECT_EQ(written.at("extension"), original.at("extension"));
    EXPECT_EQ(written.at("container_version"), P::kScheduledWeatherMetadataVersion);
    EXPECT_EQ(written.at("weatherSchedule"), script.canonical_bytes());
    EXPECT_EQ(written.at("weatherScheduleTick"), source.GetSimulationTickCount());
    const auto& anchor = source.GetWeatherSystem()->ScheduleAnchor();
    EXPECT_EQ(written.at("weatherScheduleAnchor"),
              nlohmann::json::array({anchor.x, anchor.y, anchor.z}));
    GameSession restored({.owner_continuation = true});
    restored.SetJobSystem(&jobs);
    restored.SetRootPath(RootString());
    restored.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(restored.CreateTransientWorld("Schedule reader", "1337", "default"));
    ASSERT_TRUE(restored.LoadWorldStateFrom(save)) << restored.GetWorldOpenError();
    ASSERT_NE(restored.GetWeatherSchedule(), nullptr);
    EXPECT_EQ(restored.GetWeatherSchedule()->canonical_bytes(), script.canonical_bytes());
    EXPECT_EQ(restored.GetSimulationTickCount(), source.GetSimulationTickCount());
    EXPECT_EQ(restored.GetWeatherSystem()->ComputeWeatherSubHash(),
              source.GetWeatherSystem()->ComputeWeatherSubHash());
}

TEST(WeatherScheduleTest, WeatherScriptCanonicalBoundsAndFailureAtomicity) {
    const auto script = ShortWeatherScript();
    ASSERT_TRUE(script.valid());
    const auto bytes = script.canonical_bytes();
    EXPECT_EQ(bytes,
              "WSC10500000000000000000000000000"
              "00000000000000020000000000"
              "000000000000000502ffffffff"
              "000000000000000802ffffffff"
              "000000000000000b0000000000"
              "010000000000000007");
    WeatherScript decoded;
    ASSERT_TRUE(WeatherScript::Decode(bytes, decoded));
    EXPECT_EQ(decoded, script);
    auto no_strike = script;
    no_strike.strike_tick.reset();
    ASSERT_TRUE(WeatherScript::Decode(no_strike.canonical_bytes(), decoded));
    EXPECT_EQ(decoded, no_strike);
    decoded = script;
    std::vector<std::string> invalid = {"",
                                        "WSC1",
                                        bytes + "0",
                                        "WSC2" + bytes.substr(4),
                                        std::string(WeatherScript::kMaxEncodedBytes + 1, '0')};
    auto invalid_encoding = bytes;
    invalid_encoding.replace(4, 2, "21");
    invalid.push_back(invalid_encoding);
    invalid_encoding = bytes;
    invalid_encoding.replace(32, 16, "0000000000000000");
    invalid.push_back(invalid_encoding);
    invalid_encoding = bytes;
    invalid_encoding.replace(22, 2, "05");
    invalid.push_back(invalid_encoding);
    invalid_encoding = bytes;
    invalid_encoding.replace(136, 2, "02");
    invalid.push_back(invalid_encoding);
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        invalid.push_back(bytes.substr(0, size));
    }
    for (const auto offset : {4u, 22u, 24u, 136u}) {
        auto corrupt = bytes;
        corrupt.at(offset) = 'G';
        invalid.push_back(corrupt);
    }
    for (const auto& corrupt : invalid) {
        EXPECT_FALSE(WeatherScript::Decode(corrupt, decoded));
        EXPECT_EQ(decoded, script);
    }
    std::vector<WeatherScript> refused;
    auto bad = script;
    bad.phases.clear();
    refused.push_back(bad);
    bad = script;
    bad.phases.resize(WeatherScript::kMaxPhases + 1);
    refused.push_back(bad);
    bad = script;
    bad.phases[1].start_tick = 0;
    refused.push_back(bad);
    bad = script;
    bad.phases[2].start_tick = 1;
    refused.push_back(bad);
    bad = script;
    bad.phases.back().start_tick = WeatherScript::kMaxSpanTicks + 1;
    refused.push_back(bad);
    bad = script;
    bad.phases.back().start_tick = std::numeric_limits<std::uint64_t>::max();
    refused.push_back(bad);
    bad = script;
    bad.phases[1].category = static_cast<WeatherKind>(255);
    refused.push_back(bad);
    for (const auto tick :
         {std::uint64_t{0}, std::uint64_t{12}, std::numeric_limits<std::uint64_t>::max()}) {
        bad = script;
        bad.strike_tick = tick;
        refused.push_back(bad);
    }
    GameSession session;
    const auto unscheduled = session.SystemConfigIdentity();
    ASSERT_TRUE(session.SetWeatherSchedule(script));
    const auto scheduled = session.SystemConfigIdentity();
    EXPECT_NE(scheduled, unscheduled);
    GameSession changed;
    auto changed_script = script;
    changed_script.strike_tick = 6;
    ASSERT_TRUE(changed.SetWeatherSchedule(changed_script));
    EXPECT_NE(changed.SystemConfigIdentity(), scheduled);
    changed_script = script;
    changed_script.phases[2].storm = 65534;
    ASSERT_TRUE(changed.SetWeatherSchedule(changed_script));
    EXPECT_NE(changed.SystemConfigIdentity(), scheduled);
    changed_script.phases.clear();
    EXPECT_EQ(changed.GetWeatherSchedule()->phases.size(), script.phases.size());
    for (const auto& candidate : refused) {
        EXPECT_FALSE(candidate.valid());
        EXPECT_TRUE(candidate.canonical_bytes().empty());
        EXPECT_FALSE(session.SetWeatherSchedule(candidate));
        EXPECT_EQ(session.SystemConfigIdentity(), scheduled);
    }
    WeatherScript maximal;
    for (std::uint64_t i = 0; i < WeatherScript::kMaxPhases; ++i) {
        maximal.phases.push_back({i, WeatherKind::Clear, 0, 0});
    }
    maximal.phases.back().start_tick = WeatherScript::kMaxSpanTicks;
    maximal.strike_tick = WeatherScript::kMaxSpanTicks;
    ASSERT_TRUE(maximal.valid());
    EXPECT_EQ(maximal.canonical_bytes().size(), WeatherScript::kMaxEncodedBytes);
    ASSERT_TRUE(WeatherScript::Decode(maximal.canonical_bytes(), decoded));
    EXPECT_EQ(decoded, maximal);
}

TEST(WeatherScheduleTest, InstallationSamplesBothBackendsAndLocksEveryUpdateEntry) {
    const auto script = ShortWeatherScript();
    EXPECT_FLOAT_EQ(script.Sample(3).precip_intensity, 21845.0f / 65535.0f);
    EXPECT_EQ(script.Sample(5).category, WeatherKind::Rain);
    EXPECT_EQ(script.Sample(12).precip_intensity, 0.0f);
    for (const std::uint64_t sample_tick : {0u, 5u}) {
        for (const bool regional : {false, true}) {
            for (int entry = 0; entry < 8; ++entry) {
                SCOPED_TRACE(testing::Message() << regional << '/' << entry);
                Systems::WeatherSystem weather(1337), ordinary(1337);
                Systems::WindFieldSystem wind(1337);
                if (regional) {
                    for (auto* system : {&weather, &ordinary}) {
                        system->BindRegionalBackend(
                            [](const Vec3&) { return Systems::WeatherSample{}; },
                            [] { return std::string("regional-weather"); });
                    }
                }
                const auto ordinary_hash = ordinary.ComputeWeatherSubHash();
                auto invalid = script;
                invalid.phases.clear();
                EXPECT_FALSE(weather.SetSchedule(invalid));
                ASSERT_TRUE(weather.SetSchedule(script));
                const auto installed_hash = weather.ComputeWeatherSubHash();
                EXPECT_FALSE(weather.SetSchedule(invalid));
                EXPECT_EQ(weather.ComputeWeatherSubHash(), installed_hash);
                EXPECT_NE(weather.ComputeWeatherSubHash(), ordinary_hash);
                switch (entry) {
                    case 0:
                        weather.Update(sample_tick, Vec3(0.0f), &wind);
                        break;
                    case 1:
                        weather.UpdateFromClock(sample_tick, Vec3(0.0f), wind);
                        break;
                    case 2:
                        weather.RestoreAtTick(sample_tick, Vec3(0.0f), wind);
                        break;
                    case 3:
                        weather.RefreshRegionalWeather(sample_tick, Vec3(0.0f));
                        break;
                    case 4:
                        weather.RefreshRegionalWeather(sample_tick, Vec3(0.0f), {});
                        break;
                    case 5:
                    case 6:
                        weather.AdoptRegionalWeather(
                            sample_tick, Vec3(0.0f), {}, {}, regional, entry == 6);
                        break;
                    case 7:
                        weather.Update(sample_tick, Vec3(0.0f), nullptr);
                        break;
                    default:
                        FAIL() << "uncovered entry point";
                }
                const auto expected = script.Sample(sample_tick);
                EXPECT_EQ(weather.CategoryAt(Vec3(1.0f)), expected.category);
                EXPECT_EQ(weather.PrecipitationAt(Vec3(1.0f)), expected.precip_intensity);
                EXPECT_EQ(weather.SampleAt(Vec3(1.0f)).storm_intensity, expected.storm_intensity);
                const auto hash = weather.ComputeWeatherSubHash();
                EXPECT_FALSE(weather.SetSchedule(script));
                EXPECT_EQ(weather.ComputeWeatherSubHash(), hash);
                // The declared tick constructs its single simulation event directly.
                weather.Update(7, Vec3(0.0f), nullptr);
                ASSERT_EQ(weather.StrikesThisTick().size(), 1u);
                EXPECT_EQ(weather.live_strike_count(), 1);
                weather.Update(8, Vec3(0.0f), nullptr);
                EXPECT_TRUE(weather.StrikesThisTick().empty());
                EXPECT_EQ(ordinary.ComputeWeatherSubHash(), ordinary_hash);
            }
        }
    }
}

TEST_F(WorldOpenRefusal, RemovedScheduleCannotReturnAfterSavingLegacySnapshot) {
    const auto legacy_bytes = Read(save / "world_info.json");
    ASSERT_FALSE(nlohmann::json::parse(legacy_bytes).contains("renderCalendar"));
    for (const bool prepared : {false, true}) {
        for (const bool full_metadata : {false, true}) {
            SCOPED_TRACE(testing::Message() << prepared << '/' << full_metadata);
            GameSession scheduled, reopened;
            for (auto* session : {&scheduled, &reopened}) {
                session->SetRootPath(RootString());
                session->SetJobSystem(&jobs);
                session->SetActiveRegionsEnabled(false);
            }
            ASSERT_TRUE(scheduled.SetWeatherSchedule(ShortWeatherScript()));
            ASSERT_TRUE(scheduled.CreateWorld("Schedule removal", "1337", "default"));
            ASSERT_TRUE(scheduled.SaveWorld());
            const auto destination = scheduled.GetWorldSaveDir();
            const auto initial = nlohmann::json::parse(Read(destination / "world_info.json"));
            ASSERT_EQ(initial.at("container_version"), P::kScheduledWeatherMetadataVersion);
            ASSERT_TRUE(initial.contains("weatherScheduleTick"));
            if (prepared) {
                const auto state = GameSession::PrepareWorldState(
                    scheduled.CaptureWorldPreparationOptions(), save);
                ASSERT_TRUE(scheduled.AdoptPreparedWorld(state)) << scheduled.GetWorldOpenError();
            } else {
                ASSERT_TRUE(scheduled.LoadWorldStateFrom(save)) << scheduled.GetWorldOpenError();
            }
            ASSERT_EQ(scheduled.GetWeatherSchedule(), nullptr);
            ASSERT_TRUE(full_metadata ? scheduled.SaveWorld() : scheduled.SaveWorldState());
            const auto metadata = nlohmann::json::parse(Read(destination / "world_info.json"));
            EXPECT_FALSE(metadata.contains("weatherSchedule"));
            EXPECT_FALSE(metadata.contains("weatherScheduleTick"));
            EXPECT_FALSE(metadata.contains("weatherScheduleAnchor"));
            EXPECT_EQ(metadata.at("container_version"),
                      metadata.contains("snapshotFormat")
                          ? WorldSaveService::kIndexedMetadataVersion
                          : WorldSaveService::kContainerVersion);
            ASSERT_TRUE(reopened.LoadWorld(scheduled.GetMetadata().worldId))
                << reopened.GetWorldOpenError();
            EXPECT_EQ(reopened.GetWeatherSchedule(), nullptr);
            EXPECT_EQ(Read(save / "world_info.json"), legacy_bytes);
        }
    }
}

TEST_F(WorldOpenRefusal, RemovedScheduleRestoresLegacyAmbientHashesRegardlessOfHistory) {
    GameSession legacy;
    legacy.SetRootPath(RootString());
    legacy.SetJobSystem(&jobs);
    legacy.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(legacy.LoadWorld("fixture")) << legacy.GetWorldOpenError();
    const auto expected = TestSupport::SessionWorldSubHashes(legacy);
    const auto expected_world = TestSupport::SessionWorldHash(legacy);
    const auto expected_wind = legacy.GetWindFieldSystem()->ComputeWindSubHash();
    const auto expected_aether = legacy.GetAetherFieldSystem()->ComputeAetherSubHash();
    for (const bool prepared : {false, true}) {
        for (const int prior_ticks : {0, 11}) {
            SCOPED_TRACE(testing::Message() << prepared << '/' << prior_ticks);
            GameSession target;
            target.SetRootPath(RootString());
            target.SetJobSystem(&jobs);
            target.SetActiveRegionsEnabled(false);
            ASSERT_TRUE(target.SetWeatherSchedule(ShortWeatherScript()));
            ASSERT_TRUE(target.CreateTransientWorld("Ambient reset", "1337", "default"));
            for (int tick = 0; tick < prior_ticks; ++tick) {
                ASSERT_EQ(target.TickSimulation(target.GetSimulationClock().fixed_dt()), 1u);
            }
            if (prior_ticks != 0) {
                EXPECT_NE(target.GetWindFieldSystem()->ComputeWindSubHash(), expected_wind);
                EXPECT_NE(target.GetAetherFieldSystem()->ComputeAetherSubHash(), expected_aether);
            }
            if (prepared) {
                const auto state =
                    GameSession::PrepareWorldState(target.CaptureWorldPreparationOptions(), save);
                ASSERT_TRUE(target.AdoptPreparedWorld(state)) << target.GetWorldOpenError();
            } else {
                ASSERT_TRUE(target.LoadWorldStateFrom(save)) << target.GetWorldOpenError();
            }
            ASSERT_EQ(target.GetWeatherSchedule(), nullptr);
            EXPECT_EQ(target.GetSimulationClock().tick_count(), 0u);
            const auto actual = TestSupport::SessionWorldSubHashes(target);
            EXPECT_EQ(actual.at("weather"), expected.at("weather"));
            EXPECT_EQ(actual.at("wind"), expected.at("wind"));
            EXPECT_EQ(actual.at("aether"), expected.at("aether"));
            EXPECT_EQ(target.GetAetherFieldSystem()->ComputeAetherSubHash(), expected_aether);
            EXPECT_EQ(TestSupport::SessionWorldHash(target), expected_world);
        }
    }
}

TEST_F(WorldOpenRefusal, ClockAmbientAnchorsRefuseOutOfDomainCoordinatesBeforeAdoption) {
    const auto input = root / "ambient-input";
    nlohmann::json metadata = {{"container_version", WorldSaveService::kContainerVersion},
                               {"seed", "1337"}};
    world::WorldClock(4).write_metadata(metadata);
    GameSession live;
    live.SetRootPath(RootString());
    live.SetJobSystem(&jobs);
    live.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(live.SetWeatherSchedule(ShortWeatherScript()));
    ASSERT_TRUE(live.CreateTransientWorld("Anchor refusal", "1337", "default"));
    live.SetReplicatedSimulationAnchors({});
    ASSERT_EQ(live.TickSimulation(live.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_FALSE(live.GetFieldPages().has_ambient());
    const auto before = TestSupport::SessionWorldSubHashes(live);
    const auto before_world = TestSupport::SessionWorldHash(live);
    for (const char* key : {"spawnPoint", "ambientFieldAnchor"}) {
        for (const char* axis : {"x", "z"}) {
            // The simulation domain includes -2^24 and the last float below +2^24.
            for (const float coordinate : {-16777216.0f, 0.0f, 16777215.0f}) {
                SCOPED_TRACE(testing::Message() << key << '/' << axis << '/' << coordinate);
                auto valid = metadata;
                valid[key] = {{"x", 0.0f}, {"y", 0.0f}, {"z", 0.0f}};
                valid[key][axis] = coordinate;
                Write(input / "world_info.json", valid.dump());
                ASSERT_TRUE(WorldSaveService::validate_save(input));
                world::WorldClock clock;
                bool required = false;
                std::optional<Vec3> anchor;
                ASSERT_TRUE(WorldSaveService::read_clock_metadata(
                    input, clock, required, nullptr, &anchor));
                ASSERT_TRUE(anchor.has_value());
                EXPECT_EQ(axis[0] == 'x' ? anchor->x : anchor->z, coordinate);
                EXPECT_TRUE(required);
                EXPECT_EQ(clock.tick(), 4u);
            }
            const auto valid_bytes = Read(input / "world_info.json");
            for (const double coordinate :
                 {-1e30, 1e30, -16777218.0, -16777217.0, 16777215.9, 16777216.0, 16777217.0}) {
                SCOPED_TRACE(testing::Message() << key << '/' << axis << '/' << coordinate);
                auto invalid = metadata;
                invalid[key] = {{"x", 0.0f}, {"y", 0.0f}, {"z", 0.0f}};
                invalid[key][axis] = coordinate;
                // Writers must refuse without changing the installed metadata.
                EXPECT_FALSE(WorldSaveService::save_metadata(invalid.dump(), input));
                EXPECT_EQ(Read(input / "world_info.json"), valid_bytes);
                Write(input / "world_info.json", invalid.dump());
                const auto disk = DiskBytes(input);
                EXPECT_FALSE(WorldSaveService::validate_save(input));
                world::WorldClock clock;
                bool required = false;
                std::optional<Vec3> anchor;
                EXPECT_FALSE(WorldSaveService::read_clock_metadata(
                    input, clock, required, nullptr, &anchor));
                EXPECT_FALSE(anchor.has_value());
                const auto state =
                    GameSession::PrepareWorldState(live.CaptureWorldPreparationOptions(), input);
                EXPECT_FALSE(live.AdoptPreparedWorld(state));
                EXPECT_FALSE(live.LoadWorldStateFrom(input));
                const auto after = TestSupport::SessionWorldSubHashes(live);
                EXPECT_EQ(after.at("weather"), before.at("weather"));
                EXPECT_EQ(after.at("wind"), before.at("wind"));
                EXPECT_EQ(after.at("aether"), before.at("aether"));
                EXPECT_EQ(TestSupport::SessionWorldHash(live), before_world);
                EXPECT_EQ(live.GetSimulationClock().tick_count(), 1u);
                EXPECT_EQ(DiskBytes(input), disk);
                Write(input / "world_info.json", valid_bytes);
            }
        }
    }
}

TEST_F(WorldOpenRefusal, ScheduledCalendarRequiresNonzeroSimulationClockAtAdmission) {
    auto metadata = nlohmann::json::parse(Read(save / "world_info.json"));
    world::WorldClock(4).write_metadata(metadata);
    metadata["container_version"] = P::kScheduledWeatherMetadataVersion;
    metadata["weatherSchedule"] = ShortWeatherScript().canonical_bytes();
    metadata["weatherScheduleTick"] = 4;
    metadata["weatherScheduleAnchor"] = {0.0f, 0.0f, 0.0f};
    ASSERT_TRUE(WorldSaveService::admit_metadata_bytes(metadata.dump()));
    metadata.erase("simulationTick");
    ASSERT_TRUE(metadata.contains("calendar"));
    Write(save / "world_info.json", metadata.dump());
    const auto disk = DiskBytes(root);
    auto retained = std::make_shared<const WeatherScript>(ShortWeatherScript());
    const auto original = retained;
    std::uint64_t tick = 99;
    EXPECT_FALSE(P::ReadWeatherSchedule(save, retained, tick));
    EXPECT_EQ(retained, original);
    EXPECT_EQ(tick, 99u);
    EXPECT_FALSE(WorldSaveService::admit_metadata_bytes(metadata.dump()));
    EXPECT_FALSE(WorldSaveService::save_metadata(metadata.dump(), root / "refused-clock"));
    for (const auto mode : {P::SavedWorldValidation::Catalog,
                            P::SavedWorldValidation::Complete,
                            P::SavedWorldValidation::DeferredToPreparation}) {
        const auto inspected = P::InspectSavedWorld(root, root, "fixture", {}, mode);
        EXPECT_NE(inspected.error.find("invalid weather schedule"), std::string::npos);
    }
    EXPECT_EQ(DiskBytes(root), disk);
    // Tick zero is unambiguous even if the legacy regional clock is implicit.
    metadata["weatherScheduleTick"] = 0;
    EXPECT_TRUE(WorldSaveService::admit_metadata_bytes(metadata.dump()));
}

TEST_F(WorldOpenRefusal, ScheduleAdmissionIsAtomicAcrossReadersAndWriters) {
    const auto script = ShortWeatherScript();
    const nlohmann::json valid = {{"container_version", P::kScheduledWeatherMetadataVersion},
                                  {"weatherSchedule", script.canonical_bytes()},
                                  {"weatherScheduleTick", 4},
                                  {"weatherScheduleAnchor", {0.0f, 0.0f, 0.0f}}};
    const auto input = root / "schedule-input";
    fs::create_directory(input);
    auto retained = std::make_shared<const WeatherScript>(script);
    std::uint64_t tick = 99;
    Vec3 anchor(99.0f);
    ASSERT_TRUE(P::ReadWeatherSchedule(input, retained, tick, nullptr, {}, &anchor));
    EXPECT_EQ(anchor, Vec3(0.0f));
    EXPECT_EQ(retained, nullptr);
    EXPECT_EQ(tick, 0u);
    Write(input / "world_info.json", valid.dump());
    ASSERT_TRUE(P::ReadWeatherSchedule(input, retained, tick));
    ASSERT_NE(retained, nullptr);
    EXPECT_EQ(*retained, script);
    EXPECT_EQ(tick, 4u);
    auto extreme = valid;
    extreme["weatherScheduleAnchor"] = {-16777216.0f, -0.0f, 16777215.0f};
    Write(input / "world_info.json", extreme.dump());
    ASSERT_TRUE(P::ReadWeatherSchedule(input, retained, tick, nullptr, {}, &anchor));
    EXPECT_EQ(anchor.x, -16777216.0f);
    EXPECT_TRUE(std::signbit(anchor.y));
    EXPECT_EQ(anchor.z, 16777215.0f);
    for (const float height :
         {-std::numeric_limits<float>::max(), std::numeric_limits<float>::max()}) {
        extreme["weatherScheduleAnchor"] = {0.0f, height, 0.0f};
        Write(input / "world_info.json", extreme.dump());
        ASSERT_TRUE(P::ReadWeatherSchedule(input, retained, tick, nullptr, {}, &anchor));
        EXPECT_EQ(anchor, Vec3(0.0f, height, 0.0f));
    }
    std::vector<nlohmann::json> cases;
    for (const auto version : {1, 2, 3, P::kScheduledWeatherMetadataVersion + 1}) {
        auto bad = valid;
        bad["container_version"] = version;
        cases.push_back(bad);
    }
    for (const auto& version :
         {nlohmann::json(4.0), nlohmann::json("4"), nlohmann::json(nullptr)}) {
        auto bad = valid;
        bad["container_version"] = version;
        cases.push_back(bad);
    }
    for (const auto* field :
         {"weatherSchedule", "weatherScheduleTick", "weatherScheduleAnchor", "container_version"}) {
        auto bad = valid;
        bad.erase(field);
        cases.push_back(bad);
    }
    for (const auto& encoding :
         {nlohmann::json("WSC2"),
          nlohmann::json("WSC1"),
          nlohmann::json(0),
          nlohmann::json(""),
          nlohmann::json(std::string(WeatherScript::kMaxEncodedBytes + 1, '0'))}) {
        auto bad = valid;
        bad["weatherSchedule"] = encoding;
        cases.push_back(bad);
    }
    for (const auto& bad_tick : {nlohmann::json(-1),
                                 nlohmann::json(1.0),
                                 nlohmann::json(1e38),
                                 nlohmann::json(world::WorldClock::kTickLimit)}) {
        auto bad = valid;
        bad["weatherScheduleTick"] = bad_tick;
        cases.push_back(bad);
    }
    for (const auto& bad_anchor : {nlohmann::json(nullptr),
                                   nlohmann::json(0),
                                   nlohmann::json("0,0,0"),
                                   nlohmann::json::array(),
                                   nlohmann::json::array({0, 0}),
                                   nlohmann::json::array({0, 0, 0, 0}),
                                   nlohmann::json::object({{"x", 0}, {"y", 0}, {"z", 0}})}) {
        auto bad = valid;
        bad["weatherScheduleAnchor"] = bad_anchor;
        cases.push_back(bad);
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        for (const auto& coordinate : {nlohmann::json(nullptr),
                                       nlohmann::json(true),
                                       nlohmann::json("0"),
                                       nlohmann::json(1e38 * 10),
                                       nlohmann::json(-1e38 * 10)}) {
            auto bad = valid;
            bad["weatherScheduleAnchor"][axis] = coordinate;
            cases.push_back(bad);
        }
    }
    // Float-representable horizontal values must also stay inside the simulation domain.
    for (const std::size_t axis : {0u, 2u}) {
        for (const double coordinate :
             {-1e38, -16777218.0, -16777217.0, 16777215.9, 16777216.0, 1e38}) {
            auto bad = valid;
            bad["weatherScheduleAnchor"][axis] = coordinate;
            cases.push_back(bad);
        }
    }
    for (const auto version : {2, 3}) {
        auto bad = valid;
        bad["container_version"] = version;
        bad.erase("weatherSchedule");
        bad.erase("weatherScheduleTick");
        cases.push_back(bad);
    }
    auto mismatch = valid;
    mismatch["simulationTick"] = 3;
    cases.push_back(mismatch);
    auto invalid_calendar = valid;
    invalid_calendar["calendar"] = {{"dayLengthTicks", 0}};
    cases.push_back(invalid_calendar);
    auto absent = valid;
    absent.erase("weatherSchedule");
    absent.erase("weatherScheduleTick");
    cases.push_back(absent);
    GameSession session;
    session.SetRootPath(RootString());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(session.SetWeatherSchedule(script));
    ASSERT_TRUE(session.CreateTransientWorld("Live schedule", "1337", "default"));
    auto* const world = session.GetWorldSystem();
    const auto revision = session.WorldRevision();
    const auto identity = session.SystemConfigIdentity();
    const auto hash = session.GetWeatherSystem()->ComputeWeatherSubHash();
    for (const auto& metadata : cases) {
        SCOPED_TRACE(metadata.dump());
        Write(input / "world_info.json", metadata.dump());
        const auto disk = DiskBytes(root);
        const auto original = retained;
        anchor = Vec3(99.0f);
        EXPECT_FALSE(P::ReadWeatherSchedule(input, retained, tick, nullptr, {}, &anchor));
        EXPECT_EQ(anchor, Vec3(99.0f));
        EXPECT_EQ(retained, original);
        EXPECT_EQ(tick, 4u);
        EXPECT_FALSE(WorldSaveService::save_metadata(metadata.dump(), root / "refused-schedule"));
        EXPECT_FALSE(session.LoadWorldStateFrom(input));
        const auto prepared =
            GameSession::PrepareWorldState(session.CaptureWorldPreparationOptions(), input);
        EXPECT_FALSE(session.AdoptPreparedWorld(prepared));
        EXPECT_EQ(session.GetWorldSystem(), world);
        EXPECT_EQ(session.WorldRevision(), revision);
        EXPECT_EQ(session.SystemConfigIdentity(), identity);
        EXPECT_EQ(session.GetWeatherSystem()->ComputeWeatherSubHash(), hash);
        EXPECT_EQ(DiskBytes(root), disk);
    }
    for (const auto& bytes :
         {std::string("{"), std::string(P::SavedInputLimits::kMetadataBytes + 1, ' ')}) {
        Write(input / "world_info.json", bytes);
        const auto original = retained;
        anchor = Vec3(99.0f);
        EXPECT_FALSE(P::ReadWeatherSchedule(input, retained, tick, nullptr, {}, &anchor));
        EXPECT_EQ(anchor, Vec3(99.0f));
        EXPECT_EQ(retained, original);
        EXPECT_EQ(tick, 4u);
        EXPECT_FALSE(session.LoadWorldStateFrom(input));
        EXPECT_EQ(session.WorldRevision(), revision);
    }
    fs::remove(input / "world_info.json");
    fs::create_directory(input / "world_info.json");
    const auto original = retained;
    anchor = Vec3(99.0f);
    EXPECT_FALSE(P::ReadWeatherSchedule(input, retained, tick, nullptr, {}, &anchor));
    EXPECT_EQ(anchor, Vec3(99.0f));
    EXPECT_EQ(retained, original);
    EXPECT_EQ(tick, 4u);
    fs::remove(input / "world_info.json");
    Write(input / "world_info.json", valid.dump());
    std::stop_source cancellation;
    cancellation.request_stop();
    anchor = Vec3(99.0f);
    EXPECT_FALSE(
        P::ReadWeatherSchedule(input, retained, tick, nullptr, cancellation.get_token(), &anchor));
    EXPECT_EQ(anchor, Vec3(99.0f));
    EXPECT_EQ(retained, original);
    EXPECT_EQ(tick, 4u);
    const auto cancelled = GameSession::PrepareWorldState(
        session.CaptureWorldPreparationOptions(), input, cancellation.get_token());
    EXPECT_FALSE(session.AdoptPreparedWorld(cancelled));
    EXPECT_EQ(session.WorldRevision(), revision);
}

TEST_F(WorldOpenRefusal, SessionWeatherRefusesScheduleInstallationAfterEverySetupPath) {
    const auto script = ShortWeatherScript();
    Luminumbra::Systems::WeatherSystem standalone(1337);
    ASSERT_TRUE(standalone.SetSchedule(script));
    const auto expect_sealed = [&](GameSession& session) {
        const auto* const schedule = session.GetWeatherSchedule();
        const auto identity = session.SystemConfigIdentity();
        const auto hash = session.GetWeatherSystem()->ComputeWeatherSubHash();
        const auto precipitation = session.GetWeatherSystem()->PrecipitationAt(Vec3(0.0f));
        EXPECT_FALSE(session.GetWeatherSystem()->SetSchedule(script));
        EXPECT_EQ(session.GetWeatherSchedule(), schedule);
        EXPECT_EQ(session.SystemConfigIdentity(), identity);
        EXPECT_EQ(session.GetWeatherSystem()->ComputeWeatherSubHash(), hash);
        EXPECT_EQ(session.GetWeatherSystem()->PrecipitationAt(Vec3(0.0f)), precipitation);
    };
    const auto configure = [&](GameSession& session) {
        session.SetRootPath(RootString());
        session.SetJobSystem(&jobs);
        session.SetActiveRegionsEnabled(false);
    };
    GameSession unscheduled, scheduled, reopened;
    for (auto* session : {&unscheduled, &scheduled, &reopened}) {
        configure(*session);
    }
    ASSERT_TRUE(unscheduled.CreateTransientWorld("Unscheduled", "1337", "default"));
    {
        SCOPED_TRACE("unscheduled create");
        expect_sealed(unscheduled);
    }
    ASSERT_TRUE(scheduled.SetWeatherSchedule(script));
    ASSERT_TRUE(scheduled.CreateWorld("Sealed schedule", "1337", "default"));
    {
        SCOPED_TRACE("scheduled create");
        expect_sealed(scheduled);
    }
    ASSERT_TRUE(scheduled.SaveWorld());
    ASSERT_TRUE(reopened.LoadWorld(scheduled.GetMetadata().worldId))
        << reopened.GetWorldOpenError();
    {
        SCOPED_TRACE("load");
        expect_sealed(reopened);
    }
    for (const bool prepared : {false, true}) {
        SCOPED_TRACE(prepared ? "adopt prepared" : "load state");
        GameSession target;
        configure(target);
        ASSERT_TRUE(target.CreateTransientWorld("Sealed adoption", "1337", "default"));
        if (prepared) {
            const auto state = GameSession::PrepareWorldState(
                target.CaptureWorldPreparationOptions(), scheduled.GetWorldSaveDir());
            ASSERT_TRUE(target.AdoptPreparedWorld(state)) << target.GetWorldOpenError();
        } else {
            ASSERT_TRUE(target.LoadWorldStateFrom(scheduled.GetWorldSaveDir()))
                << target.GetWorldOpenError();
        }
        expect_sealed(target);
    }
}

TEST(WeatherScheduleTest, ScheduledUpdatesNeverExposeProceduralStormsOrPerturbWind) {
    const auto script = ShortWeatherScript();
    const Vec3 anchor(96.0f, 18.0f, -192.0f);
    const Systems::StormCell storm{.center_world = Vec2(anchor.x, anchor.z),
                                   .velocity = Vec2(25.0f),
                                   .intensity = 1.0f,
                                   .spawn_tick = 0,
                                   .lifetime_ticks = 240,
                                   .seed_salt = 1};
    for (const bool regional : {false, true}) {
        for (const bool keyed : {false, true}) {
            SCOPED_TRACE(testing::Message() << regional << '/' << keyed);
            Systems::WeatherSystem weather(1337);
            Systems::WindFieldSystem wind(1337), reference_wind(1337);
            if (regional) {
                weather.BindRegionalBackend([](const Vec3&) { return Systems::WeatherSample{}; },
                                            [] { return std::string("regional-weather"); });
            }
            ASSERT_TRUE(weather.SetSchedule(script));
            EXPECT_TRUE(weather.StormCells().empty());
            // Regional projection and compatibility adoption must not leak procedural cells.
            weather.AdoptRegionalWeather(5, anchor, {storm}, {}, keyed);
            weather.PutRegionalStorm(storm);
            EXPECT_TRUE(weather.StormCells().empty());
            EXPECT_EQ(weather.active_storm_count(), 0);
            EXPECT_EQ(weather.storm_candidates_at(anchor), 0u);
            wind.InjectStormPerturbation({Vec2(anchor.x, anchor.z), 96.0f, Vec2(25.0f), 1.0f});
            weather.UpdateFromClock(6, anchor, wind);
            reference_wind.Update(6, anchor);
            EXPECT_EQ(wind.ComputeWindSubHash(), reference_wind.ComputeWindSubHash());
            weather.Update(7, anchor, &wind);
            EXPECT_TRUE(weather.StormCells().empty());
            weather.RestoreAtTick(8, anchor, wind);
            EXPECT_TRUE(weather.StormCells().empty());
            reference_wind.Update(8, anchor);
            EXPECT_EQ(wind.ComputeWindSubHash(), reference_wind.ComputeWindSubHash());
        }
    }
}

TEST(WeatherScheduleTest, RestoresAndCopiesKeepStrikeDeliveryAndPresentationSeparate) {
    for (const bool regional : {false, true}) {
        for (const bool adopted : {false, true}) {
            for (const bool has_strike : {false, true}) {
                for (const std::uint64_t restore_tick : {6u, 7u, 8u, 9u, 10u}) {
                    SCOPED_TRACE(testing::Message() << regional << '/' << adopted << '/'
                                                    << has_strike << '/' << restore_tick);
                    auto script = ShortWeatherScript();
                    if (!has_strike) {
                        script.strike_tick.reset();
                    }
                    Systems::WeatherSystem weather(1337);
                    Systems::WindFieldSystem wind(1337);
                    if (regional) {
                        weather.BindRegionalBackend(
                            [](const Vec3&) { return Systems::WeatherSample{}; },
                            [] { return std::string("regional-weather"); });
                    }
                    ASSERT_TRUE(weather.SetSchedule(script));
                    weather.Update(7, Vec3(0.0f), &wind);
                    const auto hash_at_strike = weather.ComputeWeatherSubHash();
                    if (adopted) {
                        weather.AdoptRegionalWeather(
                            restore_tick, Vec3(0.0f), {}, {}, regional, true);
                    } else {
                        weather.RestoreAtTick(restore_tick, Vec3(0.0f), wind);
                    }
                    EXPECT_TRUE(weather.StrikesThisTick().empty());
                    EXPECT_TRUE(weather.StrikeSchedule().empty());
                    EXPECT_EQ(weather.live_strike_count(), 0);
                    if (restore_tick == 7) {
                        EXPECT_EQ(weather.ComputeWeatherSubHash(), hash_at_strike);
                    }
                    auto copied = weather;
                    auto moved = std::move(copied);
                    Systems::WeatherSystem assigned(1337), move_assigned(1337);
                    assigned = weather;
                    move_assigned = std::move(assigned);
                    for (auto* system : {&weather, &moved, &move_assigned}) {
                        // Repeated access/update at the restored tick cannot redeliver.
                        system->Update(restore_tick, Vec3(0.0f), &wind);
                        EXPECT_TRUE(system->StrikesThisTick().empty());
                        EXPECT_TRUE(system->StrikeSchedule().empty());
                        if (restore_tick == 6) {
                            system->Update(7, Vec3(0.0f), &wind);
                            EXPECT_EQ(system->StrikesThisTick().size(), has_strike ? 1u : 0u);
                            auto presented = *system;
                            auto moved_presentation = std::move(presented);
                            assigned = *system;
                            move_assigned = std::move(assigned);
                            EXPECT_EQ(moved_presentation.StrikeSchedule().size(),
                                      has_strike ? 1u : 0u);
                            EXPECT_EQ(move_assigned.StrikeSchedule().size(), has_strike ? 1u : 0u);
                        }
                        system->Update(8, Vec3(0.0f), &wind);
                        EXPECT_TRUE(system->StrikesThisTick().empty());
                        EXPECT_EQ(system->StrikeSchedule().size(),
                                  restore_tick == 6 && has_strike ? 1u : 0u);
                        system->Update(10, Vec3(0.0f), &wind);
                        EXPECT_TRUE(system->StrikeSchedule().empty());
                    }
                }
            }
        }
    }
}

TEST_F(WorldOpenRefusal, ScheduleClockWithoutCalendarDoesNotRewindAnExistingLedger) {
    const auto input = root / "schedule-ledger";
    const auto script = ShortWeatherScript();
    const nlohmann::json metadata = {{"container_version", P::kScheduledWeatherMetadataVersion},
                                     {"weatherSchedule", script.canonical_bytes()},
                                     {"weatherScheduleTick", 4},
                                     {"weatherScheduleAnchor", {0.0f, 0.0f, 0.0f}}};
    Write(input / "world_info.json", metadata.dump());
    Write(WorldSaveService::active_regions_path(input), world::ActiveRegionLedger{}.encode());
    for (const bool prepared : {false, true}) {
        GameSession target;
        target.SetRootPath(RootString());
        target.SetJobSystem(&jobs);
        target.SetActiveRegionsEnabled(true);
        ASSERT_TRUE(target.CreateTransientWorld("Schedule clock", "1337", "default"));
        if (prepared) {
            const auto state =
                GameSession::PrepareWorldState(target.CaptureWorldPreparationOptions(), input);
            ASSERT_TRUE(target.AdoptPreparedWorld(state)) << target.GetWorldOpenError();
        } else {
            ASSERT_TRUE(target.LoadWorldStateFrom(input)) << target.GetWorldOpenError();
        }
        EXPECT_EQ(target.GetSimulationTickCount(), 4u);
        EXPECT_EQ(target.GetWeatherSystem()->PrecipitationAt(Vec3(0.0f)),
                  script.Sample(4).precip_intensity);
    }
}

void ExpectScriptWeather(const Systems::WeatherSystem& weather,
                         const WeatherScript& script,
                         std::uint64_t tick,
                         bool restored = false) {
    const auto expected = script.Sample(tick);
    EXPECT_EQ(weather.SampleAt(Vec3(0.0f)).category, expected.category);
    EXPECT_EQ(weather.SampleAt(Vec3(0.0f)).precip_intensity, expected.precip_intensity);
    EXPECT_EQ(weather.SampleAt(Vec3(0.0f)).storm_intensity, expected.storm_intensity);
    const bool due = script.strike_tick && tick == *script.strike_tick;
    EXPECT_EQ(weather.StrikesThisTick().size(), due && !restored ? 1u : 0u);
    EXPECT_EQ(weather.StrikeSchedule().size(), due && !restored ? 1u : 0u);
    EXPECT_EQ(weather.live_strike_count(), due && !restored ? 1 : 0);
}

void ExpectRestoredStrike(const GameSession& target,
                          const WeatherScript& script,
                          const std::string& hash,
                          const std::string& wind_hash,
                          const std::string& aether_hash,
                          const Vec3& anchor,
                          const Systems::StrikeEvent& strike) {
    const auto* restored = target.GetWeatherSystem();
    ASSERT_NE(restored, nullptr);
    ASSERT_NO_FATAL_FAILURE(ExpectScriptWeather(*restored, script, 7, true));
    EXPECT_EQ(restored->ScheduleAnchor(), anchor);
    EXPECT_EQ(restored->ComputeWeatherSubHash(), hash);
    ASSERT_NE(target.GetWindFieldSystem(), nullptr);
    ASSERT_NE(target.GetAetherFieldSystem(), nullptr);
    EXPECT_EQ(target.GetWindFieldSystem()->last_tick(), 7u);
    EXPECT_EQ(target.GetAetherFieldSystem()->last_tick(), 7u);
    EXPECT_EQ(target.GetWindFieldSystem()->ComputeWindSubHash(), wind_hash);
    EXPECT_EQ(target.GetAetherFieldSystem()->ComputeAetherSubHash(), aether_hash);
    // Rewinding to before delivery must reproduce the original coordinates.
    auto replay = *restored;
    Systems::WindFieldSystem wind(1337);
    replay.RestoreAtTick(6, anchor, wind);
    replay.Update(7, anchor, &wind);
    const auto rebuilt = replay.StrikesThisTick();
    ASSERT_EQ(rebuilt.size(), 1u);
    EXPECT_EQ(rebuilt.front().world_x, strike.world_x);
    EXPECT_EQ(rebuilt.front().world_z, strike.world_z);
    EXPECT_EQ(rebuilt.front().storm_salt, strike.storm_salt);
}

void ExpectCompletedPresentationSuppressed(GameSession& target) {
    const auto* restored = target.GetWeatherSystem();
    ASSERT_NE(restored, nullptr);
    // Completed presentation remains suppressed through the grace window.
    target.SetReplicatedSimulationAnchors({});
    for (int index = 0; index < 3; ++index) {
        ASSERT_EQ(target.TickSimulation(target.GetSimulationClock().fixed_dt()), 1u);
        ASSERT_FALSE(target.GetRegionSchedulerFault());
        EXPECT_TRUE(restored->StrikeSchedule().empty());
        EXPECT_TRUE(restored->StrikesThisTick().empty());
    }
}

void ExpectScheduledCatchUp(GameSession& target, const fs::path& before_strike, int backend) {
    if (backend == 1) {
        ASSERT_TRUE(target.LoadWorldStateFrom(before_strike)) << target.GetWorldOpenError();
    } else {
        const auto prepared =
            GameSession::PrepareWorldState(target.CaptureWorldPreparationOptions(), before_strike);
        ASSERT_TRUE(target.AdoptPreparedWorld(prepared)) << target.GetWorldOpenError();
    }
    ASSERT_EQ(target.GetSimulationTickCount(), 6u);
    target.SetReplicatedSimulationAnchors({});
    // One render frame catches up across tick 7 and polls weather at tick 8.
    ASSERT_EQ(target.TickSimulation(target.GetSimulationClock().fixed_dt() * 2), 2u);
    ASSERT_FALSE(target.GetRegionSchedulerFault());
    const auto* caught_up = target.GetWeatherSystem();
    ASSERT_NE(caught_up, nullptr);
    ASSERT_EQ(target.GetSimulationTickCount(), 8u);
    EXPECT_TRUE(caught_up->StrikesThisTick().empty());
    ASSERT_EQ(caught_up->StrikeSchedule().size(), 1u);
    EXPECT_EQ(caught_up->StrikeSchedule().front().strike_tick, 7u);
    ASSERT_EQ(target.TickSimulation(target.GetSimulationClock().fixed_dt()), 1u);
    EXPECT_EQ(caught_up->StrikeSchedule().size(), 1u);
    ASSERT_EQ(target.TickSimulation(target.GetSimulationClock().fixed_dt()), 1u);
    EXPECT_TRUE(caught_up->StrikeSchedule().empty());
}

class WeatherAnchorSaveTest : public WorldOpenRefusal {};
INSTANTIATE_TEST_SUITE_P(WeatherAndLoadBackends,
                         WeatherAnchorSaveTest,
                         testing::Combine(testing::Values(0, 1), testing::Values(0, 1, 2)));

TEST_P(WeatherAnchorSaveTest, SavedStrikeAnchorSurvivesRespawnChangeAcrossLoadBackends) {
    const auto [regional_mode, backend] = GetParam();
    const bool regional = regional_mode != 0;
    const auto script = ShortWeatherScript();
    const Vec3 respawn(1200.25f, 31.0f, -937.5f);
    GameSession source;
    source.SetRootPath(RootString());
    source.SetJobSystem(&jobs);
    source.SetActiveRegionsEnabled(regional);
    ASSERT_TRUE(source.SetWeatherSchedule(script));
    ASSERT_TRUE(source.CreateWorld("Saved weather anchor", "1337", "default"));
    source.SetReplicatedSimulationAnchors({});
    const auto before_strike =
        root / (regional ? "regional-before-strike" : "legacy-before-strike");
    for (int index = 0; index < 7; ++index) {
        ASSERT_EQ(source.TickSimulation(source.GetSimulationClock().fixed_dt()), 1u);
        ASSERT_FALSE(source.GetRegionSchedulerFault());
        if (index == 5) {
            ASSERT_TRUE(source.SaveWorldStateTo(before_strike, nullptr, true));
        }
    }
    const auto* weather = source.GetWeatherSystem();
    ASSERT_NE(weather, nullptr);
    ASSERT_NO_FATAL_FAILURE(ExpectScriptWeather(*weather, script, 7));
    const auto strikes = weather->StrikesThisTick();
    ASSERT_EQ(strikes.size(), 1u);
    const auto hash = weather->ComputeWeatherSubHash();
    const auto wind_hash = source.GetWindFieldSystem()->ComputeWindSubHash();
    const auto aether_hash = source.GetAetherFieldSystem()->ComputeAetherSubHash();
    const auto anchor = weather->ScheduleAnchor();
    ASSERT_NE(anchor, respawn);
    // Normal exit updates respawn after the final simulation tick.
    source.SetSpawnPoint(respawn);
    ASSERT_TRUE(source.SaveWorld());
    EXPECT_EQ(weather->ComputeWeatherSubHash(), hash);
    const auto metadata = nlohmann::json::parse(Read(source.GetWorldSaveDir() / "world_info.json"));
    EXPECT_EQ(metadata.at("weatherScheduleAnchor"),
              nlohmann::json::array({anchor.x, anchor.y, anchor.z}));
    EXPECT_EQ(metadata.at("spawnPoint").at("x"), respawn.x);
    GameSession target;
    target.SetRootPath(RootString());
    target.SetJobSystem(&jobs);
    target.SetActiveRegionsEnabled(regional);
    if (backend == 0) {
        ASSERT_TRUE(target.LoadWorld(source.GetMetadata().worldId)) << target.GetWorldOpenError();
        EXPECT_EQ(target.GetMetadata().spawnPoint, respawn);
    } else {
        ASSERT_TRUE(target.CreateTransientWorld("Anchor adoption", "1337", "default"));
        target.SetSpawnPoint(respawn);
        // Adoption must replace live ambient fields as well as constructor defaults.
        ASSERT_EQ(target.TickSimulation(target.GetSimulationClock().fixed_dt() * 2), 2u);
        if (backend == 1) {
            ASSERT_TRUE(target.LoadWorldStateFrom(source.GetWorldSaveDir()))
                << target.GetWorldOpenError();
        } else {
            const auto prepared = GameSession::PrepareWorldState(
                target.CaptureWorldPreparationOptions(), source.GetWorldSaveDir());
            ASSERT_TRUE(target.AdoptPreparedWorld(prepared)) << target.GetWorldOpenError();
        }
    }
    ASSERT_NO_FATAL_FAILURE(ExpectRestoredStrike(
        target, script, hash, wind_hash, aether_hash, anchor, strikes.front()));
    ASSERT_NO_FATAL_FAILURE(ExpectCompletedPresentationSuppressed(target));
    if (backend != 0) {
        ASSERT_NO_FATAL_FAILURE(ExpectScheduledCatchUp(target, before_strike, backend));
    }
}

TEST_P(WeatherAnchorSaveTest, InitialScheduledAmbientHashesSurviveEveryLoadBackend) {
    const auto [regional_mode, backend] = GetParam();
    GameSession source, target;
    for (auto* session : {&source, &target}) {
        session->SetRootPath(RootString());
        session->SetJobSystem(&jobs);
        session->SetActiveRegionsEnabled(regional_mode != 0);
    }
    ASSERT_TRUE(source.SetWeatherSchedule(ShortWeatherScript()));
    ASSERT_TRUE(source.CreateWorld("Initial scheduled fields", "1337", "default"));
    ASSERT_EQ(source.GetSimulationTickCount(), 0u);
    ASSERT_TRUE(source.SaveWorld());
    if (backend == 0) {
        ASSERT_TRUE(target.LoadWorld(source.GetMetadata().worldId)) << target.GetWorldOpenError();
    } else {
        ASSERT_TRUE(target.CreateTransientWorld("Initial field adoption", "1337", "default"));
        ASSERT_EQ(target.TickSimulation(target.GetSimulationClock().fixed_dt() * 2), 2u);
        if (backend == 1) {
            ASSERT_TRUE(target.LoadWorldStateFrom(source.GetWorldSaveDir()))
                << target.GetWorldOpenError();
        } else {
            const auto prepared = GameSession::PrepareWorldState(
                target.CaptureWorldPreparationOptions(), source.GetWorldSaveDir());
            ASSERT_TRUE(target.AdoptPreparedWorld(prepared)) << target.GetWorldOpenError();
        }
    }
    EXPECT_EQ(target.GetSimulationTickCount(), 0u);
    EXPECT_EQ(target.GetWindFieldSystem()->last_tick(), 0u);
    EXPECT_EQ(target.GetAetherFieldSystem()->last_tick(), 0u);
    EXPECT_EQ(target.GetWeatherSystem()->ComputeWeatherSubHash(),
              source.GetWeatherSystem()->ComputeWeatherSubHash());
    EXPECT_EQ(target.GetWindFieldSystem()->ComputeWindSubHash(),
              source.GetWindFieldSystem()->ComputeWindSubHash());
    EXPECT_EQ(target.GetAetherFieldSystem()->ComputeAetherSubHash(),
              source.GetAetherFieldSystem()->ComputeAetherSubHash());
}

TEST_F(WorldOpenRefusal, ScheduledMetadataRoundTripsBothClockBackendsAndLegacyBytesStayExact) {
    const auto script = ShortWeatherScript();
    for (const bool regional : {false, true}) {
        GameSession scheduled, reopened;
        for (auto* session : {&scheduled, &reopened}) {
            session->SetRootPath(RootString());
            session->SetJobSystem(&jobs);
            session->SetActiveRegionsEnabled(regional);
            session->SetAetherStateEnabled(true);
        }
        ASSERT_TRUE(scheduled.SetWeatherSchedule(script));
        ASSERT_TRUE(scheduled.CreateWorld("Scheduled metadata", "1337", "default"));
        EXPECT_FALSE(scheduled.SetWeatherSchedule(script));
        scheduled.SetReplicatedSimulationAnchors({});
        for (int index = 0; index < 4; ++index) {
            ASSERT_EQ(scheduled.TickSimulation(scheduled.GetSimulationClock().fixed_dt()), 1u);
            ASSERT_FALSE(scheduled.GetRegionSchedulerFault());
        }
        ASSERT_TRUE(scheduled.SaveWorld());
        const auto path = scheduled.GetWorldSaveDir() / "world_info.json";
        const auto metadata = nlohmann::json::parse(Read(path));
        EXPECT_EQ(metadata.at("container_version"), P::kScheduledWeatherMetadataVersion);
        EXPECT_EQ(metadata.at("weatherScheduleTick"), 4);
        ASSERT_TRUE(reopened.LoadWorld(scheduled.GetMetadata().worldId))
            << reopened.GetWorldOpenError();
        ASSERT_NE(reopened.GetWeatherSchedule(), nullptr);
        EXPECT_EQ(*reopened.GetWeatherSchedule(), script);
        EXPECT_EQ(reopened.GetSimulationTickCount(), 4u);
        EXPECT_EQ(reopened.SystemConfigIdentity(), scheduled.SystemConfigIdentity());
        EXPECT_EQ(reopened.GetWeatherSystem()->PrecipitationAt(Vec3(0.0f)),
                  script.Sample(4).precip_intensity);
        EXPECT_EQ(reopened.GetEnergyFieldState()->next_fire_tick(),
                  scheduled.GetEnergyFieldState()->next_fire_tick());
        for (const bool prepared : {false, true}) {
            GameSession target;
            target.SetRootPath(RootString());
            target.SetJobSystem(&jobs);
            target.SetActiveRegionsEnabled(regional);
            target.SetAetherStateEnabled(true);
            ASSERT_TRUE(target.CreateTransientWorld("Schedule adoption", "1337", "default"));
            if (prepared) {
                const auto state = GameSession::PrepareWorldState(
                    target.CaptureWorldPreparationOptions(), scheduled.GetWorldSaveDir());
                ASSERT_TRUE(target.AdoptPreparedWorld(state)) << target.GetWorldOpenError();
            } else {
                ASSERT_TRUE(target.LoadWorldStateFrom(scheduled.GetWorldSaveDir()))
                    << target.GetWorldOpenError();
            }
            ASSERT_NE(target.GetWeatherSchedule(), nullptr);
            EXPECT_EQ(*target.GetWeatherSchedule(), script);
            EXPECT_EQ(target.GetSimulationTickCount(), 4u);
            EXPECT_EQ(target.GetWeatherSystem()->PrecipitationAt(Vec3(0.0f)),
                      script.Sample(4).precip_intensity);
        }
    }
    const std::string legacy = "{\"container_version\":2,\"extension\":\"retained\"}\n";
    const auto output = root / "legacy-metadata";
    fs::create_directories(output);
    ASSERT_TRUE(WorldSaveService::save_metadata(legacy, output));
    EXPECT_EQ(Read(output / "world_info.json"), legacy);
    const std::string obsolete = "{\"container_version\":1,\"extension\":\"retained\"}\n";
    const auto obsolete_output = root / "obsolete-metadata";
    fs::create_directories(obsolete_output);
    ASSERT_TRUE(WorldSaveService::save_metadata(obsolete, obsolete_output));
    EXPECT_EQ(Read(obsolete_output / "world_info.json"), obsolete);
    const nlohmann::json indexed_metadata = {
        {"container_version", WorldSaveService::kIndexedMetadataVersion},
        {"snapshotFormat", 1},
        {"extension", "retained"}};
    const auto indexed_bytes = indexed_metadata.dump(4) + "\n";
    const auto indexed_output = root / "indexed-metadata";
    fs::create_directories(indexed_output);
    ASSERT_TRUE(WorldSaveService::save_metadata(indexed_bytes, indexed_output));
    EXPECT_EQ(Read(indexed_output / "world_info.json"), indexed_bytes);
    ASSERT_TRUE(WorldSaveService::save_metadata(indexed_bytes, indexed_output));
    EXPECT_EQ(Read(indexed_output / "world_info.json"), indexed_bytes);
}
TEST_F(WorldOpenRefusal, ServerOwnerContinuationRestoresInPlayerOrderAndContinues) {
    Write(root / "data/common/systems.json",
          R"({"sim":{"active_regions":{"enabled":false},"water_high_res":{"enabled":false}}})");
    Server::ServerWorldRunnerConfig source_config;
    source_config.root_path = RootString();
    source_config.world_id = "fixture";
    source_config.surface_radius = 0;
    source_config.collision_radius = 0;
    source_config.test_streaming_radius_cap = 1;
    source_config.autosave_interval_ticks = 0;
    source_config.finite_hydrology = false;
    source_config.avatar_count = 1;
    Server::ServerWorldRunner source(source_config);
    World::PlayerAvatarOwnerState captured;
    EXPECT_FALSE(source.CaptureAvatarOwner(0, captured));
    EXPECT_FALSE(source.RestoreAvatarOwner(captured));
    ASSERT_TRUE(source.Boot()) << source.GetBootError();
    ASSERT_TRUE(source.CaptureAvatarOwner(0, captured));
    const auto original = captured;
    EXPECT_FALSE(source.CaptureAvatarOwner(1, captured));
    EXPECT_EQ(captured.physics.bytes, original.physics.bytes);
    EXPECT_EQ(captured.physics.size, original.physics.size);

    auto target_config = source_config;
    target_config.avatar_count = 0;
    Server::ServerWorldRunner target(target_config);
    ASSERT_TRUE(target.Boot()) << target.GetBootError();
    auto invalid = captured;
    invalid.avatar.player_id = 1;
    EXPECT_FALSE(target.RestoreAvatarOwner(invalid));
    invalid = captured;
    invalid.avatar.velocity.x = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(target.RestoreAvatarOwner(invalid));
    ASSERT_TRUE(target.Avatars().empty());
    ASSERT_EQ(target.Session()->GetPhysicsSystem()->avatar_character_slot_count(), 0u);
    ASSERT_TRUE(target.RestoreAvatarOwner(captured));
    ASSERT_EQ(target.Avatars().size(), 1u);
    EXPECT_FALSE(target.RestoreAvatarOwner(captured));
    EXPECT_EQ(target.Avatars().size(), 1u);
    for (unsigned tick = 0; tick < 3; ++tick) {
        World::PlayerAvatarOwnerState expected;
        World::PlayerAvatarOwnerState actual;
        ASSERT_TRUE(source.CaptureAvatarOwner(0, expected));
        ASSERT_TRUE(target.CaptureAvatarOwner(0, actual));
        std::array<std::uint8_t, World::kMaxAvatarOwnerBytes> expected_bytes{};
        std::array<std::uint8_t, World::kMaxAvatarOwnerBytes> actual_bytes{};
        std::size_t expected_size = 0;
        std::size_t actual_size = 0;
        ASSERT_TRUE(World::EncodeAvatarOwner(expected, expected_bytes, expected_size));
        ASSERT_TRUE(World::EncodeAvatarOwner(actual, actual_bytes, actual_size));
        EXPECT_EQ(expected_size, actual_size);
        EXPECT_EQ(expected_bytes, actual_bytes);
        ASSERT_EQ(source.RunFixedTicks(1).ticks_executed, 1u);
        ASSERT_EQ(target.RunFixedTicks(1).ticks_executed, 1u);
    }
    // A separately spawned controller cannot silently shift the player-ID slots.
    target.Session()->GetPhysicsSystem()->create_avatar_character(Vec3(8, 40, 8));
    captured.avatar.player_id = 1;
    EXPECT_FALSE(target.RestoreAvatarOwner(captured));
    EXPECT_EQ(target.Avatars().size(), 1u);
    EXPECT_EQ(target.Session()->GetPhysicsSystem()->avatar_character_slot_count(), 2u);
}
void ExpectAvatarRestoreRefusedWithoutMutation(Server::ServerWorldRunner& target,
                                               const World::PlayerAvatarOwnerState& captured,
                                               bool through_runner) {
    auto* physics = target.Session()->GetPhysicsSystem();
    ASSERT_NE(physics, nullptr);
    const auto hash_before = target.ComputeWorldHash();
    const auto* roster_data = target.Avatars().data();
    const auto roster_capacity = target.Avatars().capacity();
    World::PlayerAvatarOwnerState owner_before;
    ASSERT_TRUE(target.CaptureAvatarOwner(0, owner_before));
    Systems::PhysicsSystem::BodySetState bodies_before;
    ASSERT_TRUE(physics->capture_body_owners(bodies_before));
    World::PlayerAvatar output{
        .player_id = 77, .position = Vec3(1), .facing = 2, .velocity = Vec3(3)};
    std::size_t index = 999;
    if (through_runner) {
        EXPECT_FALSE(target.RestoreAvatarOwner(captured));
    } else {
        EXPECT_FALSE(target.Session()->RestoreAvatarOwner(captured, output, index));
    }
    EXPECT_EQ(index, 999u);
    EXPECT_EQ(output.player_id, 77u);
    EXPECT_EQ(output.position, Vec3(1));
    EXPECT_EQ(output.facing, 2);
    EXPECT_EQ(output.velocity, Vec3(3));
    EXPECT_EQ(target.Avatars().data(), roster_data);
    EXPECT_EQ(target.Avatars().capacity(), roster_capacity);
    ASSERT_EQ(target.Avatars().size(), 1u);
    EXPECT_EQ(physics->avatar_character_slot_count(), 1u);
    World::PlayerAvatarOwnerState owner_after;
    ASSERT_TRUE(target.CaptureAvatarOwner(0, owner_after));
    // Each owner-codec scratch buffer is bounded to kMaxAvatarOwnerBytes.
    std::array<std::uint8_t, World::kMaxAvatarOwnerBytes> before_bytes{};
    std::array<std::uint8_t, World::kMaxAvatarOwnerBytes> after_bytes{};
    std::size_t before_size = 0;
    std::size_t after_size = 0;
    ASSERT_TRUE(World::EncodeAvatarOwner(owner_before, before_bytes, before_size));
    ASSERT_TRUE(World::EncodeAvatarOwner(owner_after, after_bytes, after_size));
    EXPECT_EQ(before_size, after_size);
    EXPECT_EQ(before_bytes, after_bytes);
    Systems::PhysicsSystem::BodySetState bodies_after;
    ASSERT_TRUE(physics->capture_body_owners(bodies_after));
    EXPECT_EQ(bodies_before.slot_count, bodies_after.slot_count);
    EXPECT_EQ(bodies_before.free_slots, bodies_after.free_slots);
    ASSERT_EQ(bodies_before.owners.size(), bodies_after.owners.size());
    for (std::size_t slot = 0; slot < bodies_before.owners.size(); ++slot) {
        const auto& before = bodies_before.owners[slot];
        const auto& after = bodies_after.owners[slot];
        EXPECT_EQ(before.size, after.size);
        EXPECT_EQ(before.bytes, after.bytes);
        EXPECT_EQ(before.suspended, after.suspended);
        EXPECT_EQ(before.retained_velocity, after.retained_velocity);
        EXPECT_EQ(before.retained_angular_velocity, after.retained_angular_velocity);
        EXPECT_EQ(before.was_active, after.was_active);
        EXPECT_EQ(before.registration_slot, after.registration_slot);
    }
    EXPECT_EQ(target.ComputeWorldHash(), hash_before);
    EXPECT_EQ(target.TickCount(), 1u);
}

TEST_F(WorldOpenRefusal, AvatarRestoreRefusesAfterFirstTickWithoutMutation) {
    for (const bool regional : {false, true}) {
        Write(
            root / "data/common/systems.json",
            regional
                ? R"({"sim":{"active_regions":{"enabled":true},"water_high_res":{"enabled":false}}})"
                : R"({"sim":{"active_regions":{"enabled":false},"water_high_res":{"enabled":false}}})");
        Server::ServerWorldRunnerConfig config;
        config.root_path = RootString();
        config.world_id = "fixture";
        config.surface_radius = 0;
        config.collision_radius = 0;
        config.test_streaming_radius_cap = 1;
        config.autosave_interval_ticks = 0;
        config.finite_hydrology = false;
        config.avatar_count = 1;
        Server::ServerWorldRunner source(config);
        ASSERT_TRUE(source.Boot()) << source.GetBootError();
        World::PlayerAvatarOwnerState captured;
        ASSERT_TRUE(source.CaptureAvatarOwner(0, captured));
        config.avatar_count = 0;
        Server::ServerWorldRunner target(config);
        ASSERT_TRUE(target.Boot()) << target.GetBootError();
        ASSERT_EQ(target.Session()->ActiveRegionsEnabled(), regional);
        ASSERT_EQ(target.TickCount(), 0u);
        ASSERT_TRUE(target.RestoreAvatarOwner(captured));
        ASSERT_EQ(target.RunFixedTicks(1).ticks_executed, 1u);
        ASSERT_EQ(target.TickCount(), 1u);
        ASSERT_TRUE(target.CaptureAvatarOwner(0, captured));
        captured.avatar.player_id = 1;
        ASSERT_TRUE(World::ValidateAvatarOwner(captured));
        for (const bool through_runner : {false, true}) {
            ExpectAvatarRestoreRefusedWithoutMutation(target, captured, through_runner);
        }
    }
}

TEST_F(WorldOpenRefusal, ServerFrozenRosterRestoresSequentiallyAndContinues) {
    Write(root / "data/common/systems.json",
          R"({"sim":{"active_regions":{"enabled":false},"water_high_res":{"enabled":false}}})");
    Server::ServerWorldRunnerConfig source_config;
    source_config.root_path = RootString();
    source_config.world_id = "fixture";
    source_config.surface_radius = 0;
    source_config.collision_radius = 0;
    source_config.test_streaming_radius_cap = 1;
    source_config.autosave_interval_ticks = 0;
    source_config.finite_hydrology = false;
    source_config.avatar_count = 0;
    Server::ServerWorldRunner source(source_config);
    ASSERT_TRUE(source.Boot()) << source.GetBootError();
    auto* original = source.Session()->GetPhysicsSystem();
    ASSERT_NE(original, nullptr);
    std::vector<World::PlayerAvatarOwnerState> captured(3);
    std::vector<World::PhysicsOwnerState> physics(3);
    // Seed controllers through the session bridge while keeping both zero-avatar
    // boot terrain sets identical. This setup is complete before the capture.
    for (std::size_t slot = 0; slot < captured.size(); ++slot) {
        World::PlayerAvatar avatar;
        avatar.player_id = static_cast<std::uint32_t>(slot);
        avatar.position = source.Session()->GetMetadata().spawnPoint +
                          Vec3(0, 20.0f + 8.0f * static_cast<float>(slot), 0);
        ASSERT_EQ(original->create_avatar_character(avatar.position), slot);
        ASSERT_TRUE(source.Session()->CaptureAvatarOwner(avatar, slot, captured[slot]));
    }
    original->clear_avatar_characters();
    for (const auto& owner : captured) {
        ASSERT_TRUE(source.RestoreAvatarOwner(owner));
    }
    for (std::size_t slot = 0; slot < captured.size(); ++slot) {
        original->set_avatar_frozen(slot, true, original->get_avatar_position(slot));
    }
    for (std::size_t slot = 0; slot < captured.size(); ++slot) {
        ASSERT_TRUE(source.CaptureAvatarOwner(slot, captured[slot]));
        physics[slot] = captured[slot].physics;
    }
    auto target_config = source_config;
    target_config.avatar_count = 0;
    Server::ServerWorldRunner target(target_config);
    ASSERT_TRUE(target.Boot()) << target.GetBootError();
    for (std::size_t slot = 0; slot < captured.size(); ++slot) {
        ASSERT_TRUE(
            target.RestoreAvatarOwner(captured[slot], std::span(physics).subspan(slot + 1)));
        EXPECT_EQ(target.Avatars().size(), slot + 1);
    }
    for (unsigned tick = 0; tick < 6; ++tick) {
        for (std::size_t slot = 0; slot < captured.size(); ++slot) {
            World::PlayerAvatarOwnerState expected;
            World::PlayerAvatarOwnerState actual;
            ASSERT_TRUE(source.CaptureAvatarOwner(slot, expected));
            ASSERT_TRUE(target.CaptureAvatarOwner(slot, actual));
            std::array<std::uint8_t, World::kMaxAvatarOwnerBytes> expected_bytes{};
            std::array<std::uint8_t, World::kMaxAvatarOwnerBytes> actual_bytes{};
            std::size_t expected_size = 0;
            std::size_t actual_size = 0;
            ASSERT_TRUE(World::EncodeAvatarOwner(expected, expected_bytes, expected_size));
            ASSERT_TRUE(World::EncodeAvatarOwner(actual, actual_bytes, actual_size));
            EXPECT_EQ(expected_size, actual_size);
            EXPECT_EQ(expected_bytes, actual_bytes);
            if (tick == 2) {
                original->set_avatar_frozen(slot, false, original->get_avatar_position(slot));
                auto* restored = target.Session()->GetPhysicsSystem();
                restored->set_avatar_frozen(slot, false, restored->get_avatar_position(slot));
            }
        }
        ASSERT_EQ(source.RunFixedTicks(1).ticks_executed, 1u);
        ASSERT_EQ(target.RunFixedTicks(1).ticks_executed, 1u);
    }
}
} // namespace

TEST(OwnerContinuationEnvelope, VersionsRoundTripAndMalformedExtensionsLeaveOutputUnchanged) {
    Persistence::OwnerContinuation record;
    record.preset_identity = "0123456789abcdef";
    record.simulation_tick = 60;
    record.resident_chunks = {1, 2};
    record.ambient.emplace();
    auto& ambient = *record.ambient;
    ambient.tick = 60;
    ambient.constructor_state = false;
    ambient.anchor = {8, 0, 8};
    ambient.storms.push_back({.center_world = {3, 4},
                              .velocity = {1, -0.0f},
                              .intensity = 0.5f,
                              .spawn_tick = 45,
                              .lifetime_ticks = 240,
                              .seed_salt = 7});
    ambient.strikes.push_back(
        {.strike_tick = 60, .world_x = 3, .world_z = 4, .magnitude = 0.75f, .storm_salt = 7});
    // Equal tick/salt ties use IEEE bits, which put positive coordinates first.
    ambient.strikes.push_back(
        {.strike_tick = 60, .world_x = -3, .world_z = 4, .magnitude = 0.75f, .storm_salt = 7});
    ambient.strikes.push_back(ambient.strikes.back()); // Preserve equal-key multiplicity.
    ambient.wind.push_back(
        {.center_world = {2, 4}, .radius_m = 96, .velocity = {1, -0.0f}, .intensity = 0.4f});
    std::string encoded;
    ASSERT_TRUE(Persistence::EncodeOwnerContinuation(record, encoded));
    Persistence::OwnerContinuation decoded;
    ASSERT_TRUE(Persistence::DecodeOwnerContinuation(encoded, decoded));
    std::string roundtrip;
    ASSERT_TRUE(Persistence::EncodeOwnerContinuation(decoded, roundtrip));
    EXPECT_EQ(roundtrip, encoded);
    const auto valid = nlohmann::json::parse(encoded);
    EXPECT_EQ(valid.at("version"), 2u);
    std::vector<nlohmann::json> invalid;
    const auto mutate = [&](const auto& change) {
        auto value = valid;
        change(value);
        invalid.push_back(std::move(value));
    };
    mutate([](auto& value) { value["version"] = 3u; });
    mutate([](auto& value) { value.erase("resident_count"); });
    mutate([](auto& value) { value.erase("resident_chunks"); });
    mutate([](auto& value) { value["resident_count"] = UINT64_MAX; });
    mutate([](auto& value) {
        value["resident_count"] = Persistence::kOwnerContinuationResidentChunks + 1;
    });
    mutate([](auto& value) { value["resident_count"] = 1u; });
    mutate([](auto& value) { value["resident_chunks"] = {1u, 1u}; });
    mutate([](auto& value) { value["resident_chunks"] = {2u, 1u}; });
    mutate([](auto& value) { value["resident_chunks"][0] = -1; });
    mutate([](auto& value) { value["resident_chunks"] = "unknown"; });
    mutate([](auto& value) { value.erase("ambient"); });
    mutate([](auto& value) { value["ambient"] = "unknown"; });
    mutate([](auto& value) { value["ambient"]["tick"] = 59u; });
    mutate([](auto& value) { value["ambient"]["constructor_state"] = true; });
    mutate([](auto& value) { value["ambient"]["constructor_state"] = 0; });
    mutate([](auto& value) { value["ambient"]["anchor"] = {0u}; });
    mutate([](auto& value) { value["ambient"]["anchor"][0] = UINT64_MAX; });
    mutate([](auto& value) { value["ambient"]["storms"][0].erase(7); });
    mutate([](auto& value) { value["ambient"]["storms"][0][5] = 61u; });
    mutate([](auto& value) { value["ambient"]["storms"][0][6] = 1u; });
    mutate([](auto& value) {
        value["simulation_tick"] = value["ambient"]["tick"] = 285u;
        for (auto& strike : value["ambient"]["strikes"])
            strike[0] = 285u;
    });
    mutate(
        [](auto& value) { value["ambient"]["storms"].push_back(value["ambient"]["storms"][0]); });
    mutate([](auto& value) { value["ambient"]["storms"][0][7] = UINT64_MAX; });
    mutate([](auto& value) {
        value["ambient"]["storms"][0][0] =
            std::bit_cast<std::uint32_t>(std::numeric_limits<float>::infinity());
    });
    mutate([](auto& value) { value["ambient"]["strikes"][0].erase(4); });
    mutate([](auto& value) { value["ambient"]["strikes"][0][0] = 59u; });
    mutate([](auto& value) {
        std::swap(value["ambient"]["strikes"][0], value["ambient"]["strikes"][1]);
    });
    mutate([](auto& value) {
        value["ambient"]["strikes"][0][3] = std::bit_cast<std::uint32_t>(2.0f);
    });
    mutate([](auto& value) { value["ambient"]["wind"][0].erase(5); });
    mutate([](auto& value) { value["ambient"]["wind"][0][2] = 0u; });
    mutate([](auto& value) { value["ambient"]["wind"][0][5] = 0u; });
    for (const auto* field : {"storms", "strikes", "wind"}) {
        mutate([field](auto& value) { value["ambient"][field] = nullptr; });
        mutate([field](auto& value) {
            const auto item = value["ambient"][field][0];
            for (int i = 0; i < Systems::kMaxLiveStrikes + 1; ++i) {
                value["ambient"][field].push_back(item);
            }
        });
    }
    for (const auto& value : invalid) {
        SCOPED_TRACE(value.dump());
        EXPECT_FALSE(Persistence::DecodeOwnerContinuation(value.dump(), decoded));
        ASSERT_TRUE(Persistence::EncodeOwnerContinuation(decoded, roundtrip));
        EXPECT_EQ(roundtrip, encoded);
    }
    record.version = 1;
    record.resident_chunks.clear();
    record.ambient.reset();
    ASSERT_TRUE(Persistence::EncodeOwnerContinuation(record, encoded));
    const auto legacy = nlohmann::json::parse(encoded);
    EXPECT_FALSE(legacy.contains("resident_count"));
    EXPECT_FALSE(legacy.contains("ambient"));
    ASSERT_TRUE(Persistence::DecodeOwnerContinuation(encoded, decoded));
    EXPECT_EQ(decoded.version, 1u);
    ASSERT_TRUE(Persistence::EncodeOwnerContinuation(decoded, roundtrip));
    EXPECT_EQ(roundtrip, encoded);
}

TEST_F(WorldOpenRefusal, OwnerContinuationExtensionRefusalsPreserveAllAdoptionPaths) {
    GameSession source({.owner_continuation = true});
    source.SetJobSystem(&jobs);
    source.SetRootPath(RootString());
    ASSERT_TRUE(source.LoadWorld("fixture"));
    ASSERT_TRUE(source.SaveWorldState());
    const auto metadata = nlohmann::json::parse(Read(save / "world_info.json"));
    const auto valid = nlohmann::json::parse(metadata.at("owner_continuation").get<std::string>());
    const auto* world = source.GetWorldSystem();
    const auto* physics = source.GetPhysicsSystem();
    const auto hash = source.ComputeChunkStateHash();
    const auto revision = source.WorldRevision();
    const auto sentinel = source.GetRegistry().create();
    std::vector<nlohmann::json> invalid;
    auto changed = valid;
    changed["resident_count"] = 1u;
    changed["resident_chunks"] = {Chunk::calculate_id({9000, 0, 9000})};
    invalid.push_back(changed);
    changed = valid;
    changed["version"] = 1u;
    changed.erase("resident_count");
    changed.erase("resident_chunks");
    changed.erase("ambient");
    invalid.push_back(changed);
    changed = valid;
    changed["version"] = 3u;
    invalid.push_back(changed);
    changed = valid;
    changed["resident_count"] = UINT64_MAX;
    invalid.push_back(changed);
    changed = valid;
    changed["resident_count"] = 2u;
    changed["resident_chunks"] = {0u, 0u};
    invalid.push_back(changed);
    changed = valid;
    changed["ambient"] = {{"tick", 0u},
                          {"anchor", {0u, 0u, 0u}},
                          {"constructor_state", true},
                          {"storms", nlohmann::json::array()},
                          {"strikes", nlohmann::json::array()},
                          {"wind", nlohmann::json::array()}};
    invalid.push_back(changed); // Regional state must never take the ambient restore path.
    for (const auto& value : invalid) {
        SCOPED_TRACE(value.dump());
        auto input = metadata;
        input["owner_continuation"] = value.dump();
        Write(save / "world_info.json", input.dump());
        const auto disk = DiskBytes(root);
        EXPECT_FALSE(source.LoadWorld("fixture"));
        if (value.at("version") == 1u) {
            EXPECT_NE(source.GetWorldOpenError().find("legacy owner continuation v1"),
                      std::string::npos);
        }
        EXPECT_FALSE(source.LoadWorldStateFrom(save));
        const auto prepared =
            GameSession::PrepareWorldLoad(source.CaptureWorldPreparationOptions(), "fixture");
        EXPECT_FALSE(source.AdoptPreparedWorld(prepared));
        EXPECT_EQ(source.GetWorldSystem(), world);
        EXPECT_EQ(source.GetPhysicsSystem(), physics);
        EXPECT_EQ(source.WorldRevision(), revision);
        EXPECT_EQ(source.ComputeChunkStateHash(), hash);
        EXPECT_TRUE(source.GetRegistry().valid(sentinel));
        EXPECT_EQ(DiskBytes(root), disk);
    }
    Write(save / "world_info.json", metadata.dump());
}

TEST_F(WorldOpenRefusal, PartialOwnerResidentPayloadRefusesBeforeWritingAndAdoption) {
    for (const bool partial_sdf : {false, true}) {
        SCOPED_TRACE(partial_sdf);
        GameSession source({.owner_continuation = true});
        source.SetJobSystem(&jobs);
        source.SetRootPath(RootString());
        source.SetActiveRegionsEnabled(false);
        ASSERT_TRUE(source.CreateTransientWorld("Partial resident", "1337", "default"));
        auto chunk = std::make_shared<Chunk>(IVec3(400, 0, -400));
        chunk->set_state(ChunkState::Idle);
        chunk->heightmap_data.assign(
            static_cast<std::size_t>(CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Z + 1), 8);
        if (partial_sdf) {
            chunk->sdf_data = {1};
        } else {
            chunk->heightmap_data.resize(1);
        }
        ASSERT_TRUE(source.GetWorldSystem()->adopt_streamed_chunk(chunk));
        const auto before = DiskBytes(root);
        EXPECT_FALSE(source.SaveWorldStateTo(save, nullptr, true));
        EXPECT_EQ(DiskBytes(root), before);
        if (partial_sdf) {
            chunk->sdf_data.clear();
        } else {
            chunk->heightmap_data.resize(
                static_cast<std::size_t>(CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Z + 1), 8);
        }
        ASSERT_TRUE(source.SaveWorldStateTo(save, nullptr, true));
        auto metadata = nlohmann::json::parse(Read(save / "world_info.json"));
        const auto damaged = root / (partial_sdf ? "partial-sdf-owner" : "partial-height-owner");
        // Use an unindexed legacy container with the valid v2 reference. A stale
        // index must not hide the partial-payload branch behind a checksum refusal,
        // so drop the indexed-snapshot markers the source save wrote.
        metadata.erase("snapshotFormat");
        if (metadata.at("container_version") == WorldSaveService::kIndexedMetadataVersion) {
            metadata["container_version"] = WorldSaveService::kContainerVersion;
        }
        Write(damaged / "world_info.json", metadata.dump());
        if (partial_sdf) {
            chunk->sdf_data = {1};
        } else {
            chunk->heightmap_data.resize(1);
        }
        WorldStreamingState partial;
        ASSERT_TRUE(partial.insert_chunk(chunk));
        // A partial heightmap is a legacy payload; SDF admission already rejects a
        // partial lattice in the shared decoder. Both must preserve the live session.
        ASSERT_TRUE(WorldSaveService{}.save_world(partial, damaged));
        const auto* world = source.GetWorldSystem();
        const auto* physics = source.GetPhysicsSystem();
        const auto revision = source.WorldRevision();
        const auto disk = DiskBytes(root);
        EXPECT_FALSE(source.LoadWorldStateFrom(damaged));
        EXPECT_NE(source.GetWorldOpenError().find(partial_sdf ? "SDF lattice" : "partial resident"),
                  std::string::npos);
        const auto prepared =
            GameSession::PrepareWorldState(source.CaptureWorldPreparationOptions(), damaged);
        EXPECT_FALSE(source.AdoptPreparedWorld(prepared));
        EXPECT_EQ(source.GetWorldSystem(), world);
        EXPECT_EQ(source.GetPhysicsSystem(), physics);
        EXPECT_EQ(source.WorldRevision(), revision);
        EXPECT_EQ(DiskBytes(root), disk);
    }
}

TEST(OwnerContinuationEnvelope, AmbientWindOrderRefusalsPreserveFieldsAndEncodedOutput) {
    Systems::WeatherSystem weather(1337);
    weather.LockSchedule();
    Systems::WindFieldSystem wind(1337);
    const Systems::AmbientWindPerturbation first{
        .center_world = {1, 2}, .radius_m = 96, .velocity = {3, 4}, .intensity = 0.5f};
    Systems::AmbientContinuation state{.tick = 1,
                                       .anchor = {8, 0, 8},
                                       .constructor_state = false,
                                       .storms = {},
                                       .strikes = {},
                                       .wind = {}};
    for (const std::size_t count :
         {std::size_t{0}, std::size_t{1}, static_cast<std::size_t>(Systems::kMaxStormCells)}) {
        SCOPED_TRACE(count);
        state.wind.assign(count, first);
        ASSERT_TRUE(Systems::ValidAmbientContinuation(state));
        ASSERT_NO_THROW(weather.RestoreContinuation(state, wind));
        Systems::AmbientContinuation captured;
        ASSERT_TRUE(weather.CaptureContinuation(wind, captured));
        EXPECT_EQ(captured.wind.size(), count);
    }
    // Each accepted sort-key component must break ties in the same order as injection.
    // The remaining component, radius, is fixed to 96 by continuation validation.
    std::array<Systems::AmbientWindPerturbation, 5> later;
    later.fill(first);
    later[0].center_world.x += 1;
    later[1].center_world.y += 1;
    later[2].velocity.x += 1;
    later[3].velocity.y += 1;
    later[4].intensity += 0.25f;
    for (std::size_t component = 0; component < later.size(); ++component) {
        SCOPED_TRACE(component);
        state.wind = {first, later[component]};
        ASSERT_TRUE(Systems::ValidAmbientContinuation(state));
        ASSERT_NO_THROW(weather.RestoreContinuation(state, wind));
        const auto weather_hash = weather.ComputeWeatherSubHash();
        const auto wind_hash = wind.ComputeWindSubHash();
        Systems::WindFieldSystem injected(1337);
        injected.InjectStormPerturbation(later[component]);
        injected.InjectStormPerturbation(first);
        injected.Update(state.tick, state.anchor);
        EXPECT_EQ(injected.ComputeWindSubHash(), wind_hash);

        Persistence::OwnerContinuation record;
        record.preset_identity = "0123456789abcdef";
        record.simulation_tick = state.tick;
        record.ambient = state;
        std::string encoded;
        ASSERT_TRUE(Persistence::EncodeOwnerContinuation(record, encoded));
        std::swap(state.wind[0], state.wind[1]);
        EXPECT_FALSE(Systems::ValidAmbientContinuation(state));
        EXPECT_THROW(wind.RestoreContinuation(state), std::invalid_argument);
        EXPECT_THROW(weather.RestoreContinuation(state, wind), std::invalid_argument);
        EXPECT_EQ(weather.ComputeWeatherSubHash(), weather_hash);
        EXPECT_EQ(wind.ComputeWindSubHash(), wind_hash);
        record.ambient = state;
        auto output = encoded;
        EXPECT_FALSE(Persistence::EncodeOwnerContinuation(record, output));
        EXPECT_EQ(output, encoded);
        auto reordered = nlohmann::json::parse(encoded);
        std::swap(reordered["ambient"]["wind"][0], reordered["ambient"]["wind"][1]);
        Persistence::OwnerContinuation decoded;
        ASSERT_TRUE(Persistence::DecodeOwnerContinuation(encoded, decoded));
        EXPECT_FALSE(Persistence::DecodeOwnerContinuation(reordered.dump(), decoded));
        ASSERT_TRUE(Persistence::EncodeOwnerContinuation(decoded, output));
        EXPECT_EQ(output, encoded);
    }
}

TEST(OwnerContinuationEnvelope, AmbientCaptureTracksAppliedInputsAndRefusesIncoherentFields) {
    Systems::WeatherSystem weather(1337);
    Systems::WindFieldSystem wind(1337);
    Systems::AmbientContinuation state;
    weather.LockSchedule();
    ASSERT_TRUE(weather.CaptureContinuation(wind, state));
    EXPECT_TRUE(state.constructor_state);
    weather.Update(0, {}, &wind);
    ASSERT_TRUE(weather.CaptureContinuation(wind, state));
    EXPECT_FALSE(state.constructor_state);
    wind.InjectStormPerturbation(
        {.center_world = {1, 2}, .radius_m = 96, .velocity = {3, 4}, .intensity = 0.5f});
    wind.Update(1, {8, 0, 8});
    const auto original = state;
    EXPECT_FALSE(weather.CaptureContinuation(wind, state));
    EXPECT_EQ(state.tick, original.tick);
    EXPECT_EQ(state.constructor_state, original.constructor_state);
    weather.Update(1, {9, 0, 8}, &wind);
    EXPECT_FALSE(weather.CaptureContinuation(wind, state));
    weather.Update(1, {8, 0, 8}, &wind);
    wind.ClearStormPerturbations();
    const auto check = [&](const Systems::WindFieldSystem& value) {
        ASSERT_TRUE(weather.CaptureContinuation(value, state));
        EXPECT_FALSE(state.constructor_state);
        ASSERT_EQ(state.wind.size(), 1u);
        EXPECT_EQ(state.wind[0].center_world, Vec2(1, 2));
        EXPECT_EQ(state.wind[0].velocity, Vec2(3, 4));
    };
    ASSERT_NO_FATAL_FAILURE(check(wind));
    auto copied = wind;
    ASSERT_NO_FATAL_FAILURE(check(copied));
    auto moved = std::move(copied);
    ASSERT_NO_FATAL_FAILURE(check(moved));
    copied = wind;
    ASSERT_NO_FATAL_FAILURE(check(copied));
    moved = std::move(copied);
    ASSERT_NO_FATAL_FAILURE(check(moved));
    for (int i = 0; i < Systems::kMaxStormCells + 1; ++i) {
        wind.InjectStormPerturbation(
            {.center_world = {1, 2}, .radius_m = 96, .velocity = {3, 4}, .intensity = 0.5f});
    }
    wind.Update(1, {8, 0, 8});
    EXPECT_FALSE(weather.CaptureContinuation(wind, state));
    ASSERT_EQ(state.wind.size(), 1u);
    Systems::WeatherSystem scheduled(1337);
    ASSERT_TRUE(scheduled.SetSchedule(ShortWeatherScript()));
    EXPECT_FALSE(scheduled.CaptureContinuation(wind, state));
    EXPECT_THROW(scheduled.RestoreContinuation(state, wind), std::invalid_argument);
    const auto weather_hash = weather.ComputeWeatherSubHash();
    const auto wind_hash = wind.ComputeWindSubHash();
    state.anchor.x = std::numeric_limits<float>::infinity();
    EXPECT_THROW(weather.RestoreContinuation(state, wind), std::invalid_argument);
    EXPECT_EQ(weather.ComputeWeatherSubHash(), weather_hash);
    EXPECT_EQ(wind.ComputeWindSubHash(), wind_hash);
}

TEST_F(WorldOpenRefusal, RegionReplacementPreservesIndependentReaderImage) {
    const IVec3 coords(0, 0, 0);
    auto original = std::make_shared<Chunk>(coords);
    original->heightmap_data.assign(std::size_t{17} * 17, 3.0f);
    original->mark_sdf_loaded_or_edited();
    WorldStreamingState state;
    state.insert_chunk(original);
    ASSERT_TRUE(WorldSaveService{}.save_world(state, save));
    WorldSaveService::ChunkReader independent_reader;
    const auto old_image = WorldSaveService::read_chunk(save, coords, false, &independent_reader);
    ASSERT_NE(old_image.chunk, nullptr);
    auto replacement = std::make_shared<Chunk>(coords);
    replacement->heightmap_data.assign(std::size_t{17} * 17, 7.0f);
    replacement->mark_sdf_loaded_or_edited();
    WorldStreamingState replaced;
    replaced.insert_chunk(replacement);
    ASSERT_TRUE(WorldSaveService{}.save_world(replaced, save));
    EXPECT_EQ(old_image.chunk->heightmap_data, original->heightmap_data);
    WorldSaveService::ChunkReader new_reader;
    const auto new_image = WorldSaveService::read_chunk(save, coords, false, &new_reader);
    ASSERT_NE(new_image.chunk, nullptr);
    EXPECT_EQ(new_image.chunk->heightmap_data, replacement->heightmap_data);
    const auto refreshed = WorldSaveService::read_chunk(save, coords, false, &independent_reader);
    ASSERT_NE(refreshed.chunk, nullptr);
    EXPECT_EQ(refreshed.chunk->heightmap_data, replacement->heightmap_data);
    EXPECT_EQ(old_image.chunk->heightmap_data, original->heightmap_data);
}
