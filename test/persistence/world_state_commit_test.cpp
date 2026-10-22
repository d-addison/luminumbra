#include "../support/UniqueTempPath.h"
#include "../support/WorldgenTestData.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/world/WorldStreamingState.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {
namespace fs = std::filesystem;
using namespace Luminumbra;
using Persistence::WorldSaveService;
using world::GameSession;

class WorldStateCommit : public testing::TestWithParam<bool> {
protected:
    fs::path root;
    JobSystem jobs;

    void SetUp() override {
        root = TestSupport::CreateUniqueTempDirectory("world_state_commit_");
        fs::create_directories(root / "worlds/atlas/presets");
        fs::copy_file(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets/default.json",
                      root / "worlds/atlas/presets/default.json");
        TestSupport::CopyWorldgenTestData(LUMINUMBRA_SOURCE_ROOT, root);
        jobs.startup(1);
    }
    void TearDown() override {
        jobs.shutdown();
        std::error_code error;
        fs::remove_all(root, error);
    }
    void Configure(GameSession& session) {
        session.SetRootPath(root.string() + static_cast<char>(fs::path::preferred_separator));
        session.SetJobSystem(&jobs);
        session.SetActiveRegionsEnabled(GetParam());
    }
    static nlohmann::json Metadata(const fs::path& save) {
        std::ifstream input(save / "world_info.json", std::ios::binary);
        return nlohmann::json::parse(input);
    }
};

TEST_P(WorldStateCommit, EverySavePathPersistsCurrentCursorWithAndWithoutDirtyChunks) {
    GameSession session;
    Configure(session);
    ASSERT_TRUE(session.CreateWorld("Cursor persistence", "1337", "default"));
    const auto save = session.GetWorldSaveDir();
    const auto id = session.GetMetadata().worldId;
    auto metadata = Metadata(save);
    metadata.erase("renderCalendar"); // Exercise clock-disabled worlds without a marker.
    metadata["waterSimCursor"] = 19;
    metadata["extension"] = {{"unknown", "retained"}};
    ASSERT_TRUE(WorldSaveService::save_metadata(metadata.dump(), save));
    ASSERT_TRUE(session.LoadWorld(id)) << session.GetWorldOpenError();
    EXPECT_EQ(session.GetWorldSystem()->GetWaterSimWindowCursor(), 19u);
    if (!GetParam()) {
        EXPECT_EQ(session.GetRenderCalendar(), world::WorldCalendar::Legacy());
    }

    auto chunk = std::make_shared<Chunk>(IVec3(0, 0, 0));
    chunk->set_state(ChunkState::Idle);
    chunk->heightmap_data = {8.0f};
    ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
    // Four entry points, each with a dirty payload and then an empty dirty set.
    for (int entry = 0; entry < 4; ++entry) {
        for (const bool dirty : {true, false}) {
            SCOPED_TRACE(entry);
            SCOPED_TRACE(dirty);
            const std::size_t cursor =
                65u + static_cast<std::size_t>(entry) * 2u + (dirty ? 0u : 1u);
            session.GetWorldSystem()->SetWaterSimWindowCursor(cursor);
            if (dirty) {
                chunk->heightmap_data.front() += 1.0f;
                chunk->mark_voxel_data_dirty();
            }
            const auto before = Metadata(save);
            if (entry == 0) {
                ASSERT_TRUE(session.SaveWorldState());
            } else if (entry == 1) {
                ASSERT_TRUE(session.SaveWorldStateTo(save));
            } else if (entry == 2) {
                ASSERT_TRUE(session.SaveWorldStateTo(save, nullptr, /*full_snapshot=*/true));
            } else {
                ASSERT_TRUE(session.SaveWorld());
            }
            const auto after = Metadata(save);
            EXPECT_EQ(after.at("waterSimCursor"), cursor);
            if (!GetParam()) {
                EXPECT_FALSE(after.contains("renderCalendar"));
                EXPECT_FALSE(after.contains("calendar"));
                EXPECT_FALSE(after.contains("simulationTick"));
                if (entry < 2) {
                    EXPECT_EQ(after.at("extension"), before.at("extension"));
                }
            }
            GameSession reloaded;
            Configure(reloaded);
            ASSERT_TRUE(reloaded.LoadWorld(id)) << reloaded.GetWorldOpenError();
            EXPECT_EQ(reloaded.GetWorldSystem()->GetWaterSimWindowCursor(), cursor);
            const auto restored = reloaded.GetWorldSystem()->snapshot_streamed_chunks();
            ASSERT_EQ(restored.size(), 1u);
            EXPECT_EQ(restored.front()->heightmap_data, chunk->heightmap_data);
            EXPECT_FALSE(chunk->is_voxel_data_dirty());
        }
    }
    for (const bool dirty : {false, true}) {
        SCOPED_TRACE(dirty);
        const auto destination = root / (dirty ? "new-dirty-export" : "new-clean-export");
        session.GetWorldSystem()->SetWaterSimWindowCursor(91);
        if (dirty)
            chunk->mark_voxel_data_dirty();
        ASSERT_TRUE(session.SaveWorldStateTo(destination));
        EXPECT_EQ(Metadata(destination).at("waterSimCursor"), 91u);
        GameSession reader;
        Configure(reader);
        ASSERT_TRUE(reader.CreateTransientWorld("Export reader", "1337", "default"));
        ASSERT_TRUE(reader.LoadWorldStateFrom(destination)) << reader.GetWorldOpenError();
        EXPECT_EQ(reader.GetWorldSystem()->GetWaterSimWindowCursor(), 91u);
        if (dirty || GetParam()) {
            EXPECT_EQ(reader.GetLastLoadedChunkCount(), 1u);
        }
    }
}

TEST_P(WorldStateCommit, RefusedSnapshotAndWorldLoadsPreservePreviouslyPublishedState) {
    GameSession source;
    Configure(source);
    ASSERT_TRUE(source.CreateWorld("Refused candidate", "1337", "default"));
    const auto save = source.GetWorldSaveDir();
    const auto id = source.GetMetadata().worldId;
    auto metadata = Metadata(save);
    metadata.erase("calendar");
    metadata.erase("simulationTick");
    metadata["renderCalendar"] =
        world::WorldClock(0, world::WorldCalendar::Legacy()).calendar_metadata();
    metadata["waterSimCursor"] = 65;
    // Directly author a presentation-only save, usable with either feature setting.
    fs::remove(WorldSaveService::active_regions_path(save));
    ASSERT_TRUE(WorldSaveService::write_file_durably(save / "world_info.json", metadata.dump()));
    for (int failure = 0; failure < (GetParam() ? 5 : 2); ++failure) {
        SCOPED_TRACE(failure);
        WorldStreamingState state;
        auto chunk = state.get_or_create_chunk(IVec3(0, 0, 0));
        chunk->set_state(ChunkState::Idle);
        chunk->sdf_data = failure == 0 ? std::vector<float>{1.0f} : std::vector<float>{};
        if (failure < 2) {
            ASSERT_TRUE(WorldSaveService{}.save_world(state, save));
        }
        if (failure >= 2) {
            auto clock_metadata = metadata;
            clock_metadata.erase("renderCalendar");
            world::WorldClock(0, world::WorldCalendar::Legacy()).write_metadata(clock_metadata);
            ASSERT_TRUE(WorldSaveService::write_file_durably(save / "world_info.json",
                                                             clock_metadata.dump()));
        }
        const auto ledger_path = WorldSaveService::active_regions_path(save);
        fs::remove(ledger_path);
        if (failure == 2 || failure == 3) {
            ASSERT_TRUE(WorldSaveService::write_file_durably(ledger_path,
                                                             failure == 2 ? "partial ledger" : ""));
        }
        if (failure == 4) {
            world::ActiveRegionLedger persisted(world::RegionSchedulerConfig{10, 3, 2});
            persisted.set_local_anchor(Vec3(8, 18, 8));
            ASSERT_TRUE(WorldSaveService::write_file_durably(ledger_path, persisted.encode()));
        }
        if (failure == 1) {
            const auto region = WorldSaveService::region_file_path(save, 0, 0);
            ASSERT_TRUE(WorldSaveService::write_file_durably(region, "partial chunk"));
        }
        for (const bool whole_world : {false, true}) {
            SCOPED_TRACE(whole_world);
            GameSession reader;
            Configure(reader);
            if (failure == 4)
                reader.SetRegionSchedulerConfig(world::RegionSchedulerConfig{});
            ASSERT_TRUE(reader.CreateTransientWorld("Retained world", "1234", "default"));
            auto retained_chunk = std::make_shared<Chunk>(IVec3(9, 0, 9));
            retained_chunk->set_state(ChunkState::Idle);
            ASSERT_TRUE(reader.GetWorldSystem()->adopt_streamed_chunk(retained_chunk));
            reader.GetWorldSystem()->SetWaterSimWindowCursor(19);
            const auto calendar = reader.GetRenderCalendar();
            const auto clock = reader.GetWorldClock().canonical_bytes();
            const auto ledger = reader.GetActiveRegionLedger().canonical_bytes();
            const auto previous_id = reader.GetMetadata().worldId;
            const auto* previous_system = reader.GetWorldSystem();
            ASSERT_NE(calendar, world::WorldCalendar::Legacy());
            if (whole_world) {
                EXPECT_FALSE(reader.LoadWorld(id));
            } else {
                EXPECT_FALSE(reader.LoadWorldStateFrom(save));
            }
            EXPECT_FALSE(reader.GetWorldOpenError().empty());
            if (failure == 4) {
                EXPECT_NE(reader.GetWorldOpenError().find("explicit override"), std::string::npos);
            }
            EXPECT_EQ(reader.GetRenderCalendar(), calendar);
            EXPECT_EQ(reader.GetWorldClock().canonical_bytes(), clock);
            EXPECT_EQ(reader.GetActiveRegionLedger().canonical_bytes(), ledger);
            EXPECT_EQ(reader.GetMetadata().worldId, previous_id);
            EXPECT_EQ(reader.GetWorldSystem(), previous_system);
            EXPECT_EQ(reader.GetWorldSystem()->GetWaterSimWindowCursor(), 19u);
            const auto retained = reader.GetWorldSystem()->snapshot_streamed_chunks();
            ASSERT_EQ(retained.size(), 1u);
            EXPECT_EQ(retained.front(), retained_chunk);
        }
        // Restore the malformed container before preparing the next failure.
        fs::remove_all(WorldSaveService::region_directory(save));
        fs::remove(WorldSaveService::world_manifest_path(save));
        fs::remove(ledger_path);
    }
}

INSTANTIATE_TEST_SUITE_P(ActiveRegionsEnabledAndDisabled, WorldStateCommit, testing::Bool());
} // namespace
