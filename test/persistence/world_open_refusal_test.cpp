#include <gtest/gtest.h>

#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/FarLodStore.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_server/ServerWorldRunner.h"

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
            R"({"name":"Fixture","seed":"1337","worldType":"default","creationTime":1,"spawnPoint":{"x":8,"y":18,"z":8}})");
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
        return root.string() + fs::path::preferred_separator;
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
    const bool obsolete = kind <= 4 || kind == 11 || kind == 14 || kind == 17;
    const bool future = kind == 5 || kind == 8 || kind == 15 || kind == 21;
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
                         testing::Combine(testing::Range(0, 3), testing::Range(0, 22)));

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

} // namespace
