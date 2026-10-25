#include "../support/CalendarMetadataCases.h"
#include "../support/WorldgenTestData.h"
#include "luminumbra_client/app/SessionExit.h"
#include "luminumbra_client/ui/core/UIHotReload.h"
#include <chrono>
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
#include "luminumbra_common/persistence/SavedWorldCatalog.h"
#include "luminumbra_common/persistence/SyntheticSaveFixture.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/replay/ReplayStream.h"
#include "luminumbra_common/systems/AetherFieldSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
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

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
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
            json["schema"] = "luminumbra.persistence.world_manifest.v2";
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
                json["container_version"] = 3;
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
        if (patch.contains("calendar"))
            EXPECT_EQ(written.at("calendar"), patch.at("calendar"));
        else
            EXPECT_EQ(written.contains("renderCalendar"), patch.contains("renderCalendar"));
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
    ASSERT_TRUE(fs::remove(ledger_path));
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
    energy->Tick(0);
    if (existing_snapshot) {
        ASSERT_TRUE(session.SaveWorldState());
        ASSERT_FALSE(chunk->is_voxel_data_dirty());
    }
    ASSERT_EQ(WorldSaveService::has_chunk_snapshot(session.GetWorldSaveDir()), existing_snapshot);

    chunk->heightmap_data.front() = 42.0f;
    chunk->mark_voxel_data_dirty();
    registry.get<Components::PlantGrowthComponent>(plant).growth_points = 123;
    energy->QueueDeposit(2, 0, 0, 0, 1000);
    energy->Tick(0);
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
        energy->Tick(0);
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

// Fail the selected sibling writer after the chunk and manifest replacements.
// Windows denies replacement while still allowing validation to read the file;
// POSIX injects a directory-flush failure through the existing persistence seam.
class SiblingWriteFailure {
public:
    SiblingWriteFailure() = default;
    ~SiblingWriteFailure() {
        Reset();
    }
    SiblingWriteFailure(const SiblingWriteFailure&) = delete;
    SiblingWriteFailure& operator=(const SiblingWriteFailure&) = delete;

    void Arm(const fs::path& destination, bool energy) {
#if defined(_WIN32)
        const auto path = energy ? destination / "aether_state.efs"
                                 : WorldSaveService::plant_entities_path(destination);
        handle = ::CreateFileW(path.c_str(),
                               GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr,
                               OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL,
                               nullptr);
        ASSERT_NE(handle, INVALID_HANDLE_VALUE);
#else
        // One resident region and its manifest each flush all ancestor entries
        // and then their own directory before the selected sibling writer.
        int flushes = 2;
        for (auto ancestor = WorldSaveService::region_directory(destination);
             !ancestor.empty() && ancestor != ancestor.root_path();
             ancestor = ancestor.parent_path())
            flushes += 2;
        if (energy) {
            const auto plants = WorldSaveService::plant_entities_path(destination);
            // The plant writer flushes every ancestor entry and its replacement.
            for (auto ancestor = plants.parent_path();
                 !ancestor.empty() && ancestor != ancestor.root_path();
                 ancestor = ancestor.parent_path())
                ++flushes;
            ++flushes;
        }
        WorldSaveService::set_directory_flush_failure_for_testing(flushes);
#endif
    }

    void Reset() {
#if defined(_WIN32)
        if (handle != INVALID_HANDLE_VALUE) {
            EXPECT_TRUE(::CloseHandle(handle));
            handle = INVALID_HANDLE_VALUE;
        }
#else
        WorldSaveService::set_directory_flush_failure_for_testing(-1);
#endif
    }

private:
#if defined(_WIN32)
    HANDLE handle = INVALID_HANDLE_VALUE;
#endif
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

TEST_P(CompleteSaveFailure, SiblingWriterFailureKeepsMetadataAndDirtyChunksPending) {
    const auto [regions, failure_and_path] = GetParam();
    const bool energy_failure = failure_and_path % 2 != 0;
    const int save_path = failure_and_path / 2;
    auto config = SnapshotRunnerConfig(RootString(), regions != 0);
    if (save_path == 5)
        config.autosave_interval_ticks = 1;
    Server::ServerWorldRunner runner(config);
    ASSERT_NO_FATAL_FAILURE(BootSnapshotRunner(runner, regions != 0));
    auto* session = runner.Session();
    std::shared_ptr<Chunk> chunk;
    ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(*session, chunk, true));
    ASSERT_TRUE(session->SaveWorldState());
    const auto canonical = session->GetWorldSaveDir();
    const auto destination = save_path == 3
                                 ? root / "alternate/worlds/saves" / session->GetMetadata().worldId
                                 : canonical;
    if (save_path == 3) {
        fs::create_directories(destination.parent_path());
        fs::copy(canonical, destination, fs::copy_options::recursive);
    }
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
    session->GetEnergyFieldState()->Tick(0);

    // Declare after the runner so even a fatal assertion releases the obstruction
    // before its destructor attempts a shutdown save.
    SiblingWriteFailure failure;
    ASSERT_NO_FATAL_FAILURE(failure.Arm(destination, energy_failure));
    world::WorldStateSaveReport report;
    if (save_path == 0)
        EXPECT_FALSE(session->SaveWorldState(&report));
    else if (save_path == 1)
        EXPECT_EQ(runner.SaveFullSnapshot(), 0u);
    else if (save_path == 2)
        EXPECT_FALSE(session->SaveWorld());
    else if (save_path == 3)
        EXPECT_FALSE(session->SaveWorldStateTo(destination, &report, /*full_snapshot=*/true));
    else if (save_path == 4) {
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
    } else if (save_path == 5) {
        const auto tick_report = runner.RunFixedTicks(1);
        EXPECT_EQ(tick_report.ticks_executed, 1u);
        EXPECT_EQ(tick_report.autosave_passes, 0u);
        EXPECT_EQ(tick_report.autosave_writes, 0u);
    } else {
        runner.Shutdown(&report);
    }
    failure.Reset();
    EXPECT_FALSE(report.saved);
    EXPECT_EQ(report.chunks_saved, 0u);
    EXPECT_TRUE(chunk->is_voxel_data_dirty());
    EXPECT_EQ(Read(destination / "world_info.json"), old_metadata);
    EXPECT_EQ(Read(WorldSaveService::active_regions_path(destination)), old_ledger);
    // Prove the injection reached the sibling writer after durable chunk progress.
    EXPECT_NE(Read(WorldSaveService::region_file_path(destination, 0, 0)), old_chunks);
    EXPECT_EQ(Read(destination / "aether_state.efs"), old_energy);
    if (energy_failure)
        EXPECT_NE(Read(WorldSaveService::plant_entities_path(destination)), old_plants);
    else
        EXPECT_EQ(Read(WorldSaveService::plant_entities_path(destination)), old_plants);
    EXPECT_TRUE(WorldSaveService::validate_save(destination));
    if (save_path != 6) {
        if (save_path == 3)
            ASSERT_TRUE(session->SaveWorldStateTo(destination, nullptr, /*full_snapshot=*/true));
        else
            ASSERT_TRUE(session->SaveWorld());
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
}

// Each CTest entry exercises one writer, host entry point and region mode.
INSTANTIATE_TEST_SUITE_P(RegionModesWritersAndSavePaths,
                         CompleteSaveFailure,
                         testing::Combine(testing::Values(0, 1), testing::Range(0, 14)));

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

} // namespace
