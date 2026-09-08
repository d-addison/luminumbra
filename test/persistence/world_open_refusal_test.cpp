#include <gtest/gtest.h>

#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/persistence/SavedWorldCatalog.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/AetherFieldSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"
#include "luminumbra_common/world/FarLodStore.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_server/ServerWorldRunner.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <tuple>

#include <nlohmann/json.hpp>

namespace {
namespace fs = std::filesystem;
using namespace Luminumbra;
using Persistence::WorldSaveService;
using world::GameSession;

std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
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

class WorldOpenRefusal : public testing::TestWithParam<std::tuple<int, int>> {
protected:
    fs::path root;
    fs::path save;
    JobSystem jobs;
    void SetUp() override {
        root = fs::temp_directory_path() /
               ("world_open_refusal_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root / "worlds/atlas/presets");
        fs::copy_file(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets/default.json",
                      root / "worlds/atlas/presets/default.json");
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
        if (entry == 1)
            ASSERT_TRUE(client->CreateWorld("Client", "1337", "default"));
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
    if (obsolete)
        EXPECT_EQ(error, WorldSaveService::kObsoleteWorldMessage);
    else if (future)
        EXPECT_NE(error.find("future"), std::string::npos);
    else {
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

TEST_F(WorldOpenRefusal, ActiveClockSaveRefusesWithFeatureOffThroughEveryOpenPath) {
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
    Server::ServerWorldRunnerConfig config;
    config.root_path = RootString();
    config.world_id = "fixture";
    config.surface_radius = 0;
    config.collision_radius = 0;
    const auto before = DiskBytes(save);
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
    EXPECT_EQ(DiskBytes(save), before);
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
    cancellation.request_stop();
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
    const auto before = DiskBytes(root);
    GameSession session;
    session.SetRootPath(RootString());
    session.SetJobSystem(&jobs);
    ASSERT_TRUE(session.LoadWorld("fixture")) << session.GetWorldOpenError();
    EXPECT_EQ(session.GetLastLoadedChunkCount(), 1u);
    EXPECT_EQ(DiskBytes(root), before);
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

} // namespace
