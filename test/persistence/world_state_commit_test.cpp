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

enum class LoadFailure {
    Sdf,
    Chunk,
    PartialMetadata,
    UnreadableMetadata,
    Cursor,
    PartialLedger,
    EmptyLedger,
    Scheduler,
    ActiveRegionsRequired
};

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
    void CheckIncrementalZeroCursor(bool canonical, bool dirty) {
        SCOPED_TRACE(canonical);
        SCOPED_TRACE(dirty);
        GameSession session;
        Configure(session);
        ASSERT_TRUE(session.CreateWorld("Zero cursor", "1337", "default"));
        ASSERT_TRUE(session.LoadWorldStateFrom(root / "unmarked-snapshot"));
        const auto save = canonical ? session.GetWorldSaveDir() : root / "cursor-export";
        fs::remove_all(save);
        auto chunk = std::make_shared<Chunk>(IVec3(0, 0, 0));
        chunk->set_state(ChunkState::Idle);
        chunk->heightmap_data = {8.0f};
        ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
        const auto save_incrementally = [&]() {
            if (dirty)
                chunk->mark_voxel_data_dirty();
            return canonical ? session.SaveWorldState() : session.SaveWorldStateTo(save);
        };
        ASSERT_EQ(session.GetWorldSystem()->GetWaterSimWindowCursor(), 0u);
        ASSERT_TRUE(save_incrementally());
        EXPECT_EQ(fs::exists(save / "world_info.json"), GetParam());
        if (!GetParam()) {
            EXPECT_EQ(fs::exists(save), dirty);
            EXPECT_FALSE(fs::exists(save / "preset.json"));
            EXPECT_EQ(fs::exists(WorldSaveService::world_manifest_path(save)), dirty);
            EXPECT_EQ(fs::exists(WorldSaveService::region_file_path(save, 0, 0)), dirty);
            std::size_t files = 0;
            if (fs::exists(save)) {
                for (const auto& entry : fs::recursive_directory_iterator(save))
                    files += entry.is_regular_file() ? 1u : 0u;
            }
            EXPECT_EQ(files, dirty ? 2u : 0u);
        }
        session.GetWorldSystem()->SetWaterSimWindowCursor(65);
        ASSERT_TRUE(save_incrementally());
        EXPECT_EQ(Metadata(save).at("waterSimCursor"), 65u);
        auto metadata = Metadata(save);
        metadata["extension"] = {{"unknown", "retained"}};
        ASSERT_TRUE(WorldSaveService::save_metadata(metadata.dump(), save));
        session.GetWorldSystem()->SetWaterSimWindowCursor(0);
        ASSERT_TRUE(save_incrementally());
        const auto reset = Metadata(save);
        EXPECT_EQ(reset.at("waterSimCursor"), 0u);
        if (!GetParam()) {
            EXPECT_EQ(reset.at("extension"), metadata.at("extension"));
        }
        GameSession reader;
        Configure(reader);
        ASSERT_TRUE(reader.CreateTransientWorld("Zero reader", "1337", "default"));
        reader.GetWorldSystem()->SetWaterSimWindowCursor(19);
        ASSERT_TRUE(reader.LoadWorldStateFrom(save)) << reader.GetWorldOpenError();
        EXPECT_EQ(reader.GetWorldSystem()->GetWaterSimWindowCursor(), 0u);
        // Zero must still validate existing metadata before writing or acknowledging payloads.
        for (const bool unreadable : {false, true}) {
            SCOPED_TRACE(unreadable);
            const auto path = save / "world_info.json";
            ASSERT_TRUE(fs::remove(path));
            if (unreadable) {
                ASSERT_TRUE(fs::create_directory(path));
            } else {
                ASSERT_TRUE(WorldSaveService::write_file_durably(path, "{"));
            }
            const bool was_dirty = chunk->is_voxel_data_dirty();
            EXPECT_FALSE(save_incrementally());
            EXPECT_EQ(chunk->is_voxel_data_dirty(), dirty || was_dirty);
            EXPECT_EQ(fs::is_directory(path), unreadable);
            if (!unreadable) {
                EXPECT_EQ(fs::file_size(path), 1u);
            }
            fs::remove_all(path);
            ASSERT_TRUE(WorldSaveService::write_file_durably(path, reset.dump()));
        }
    }

    void CheckRetainedWorld(LoadFailure failure,
                            const fs::path& save,
                            const std::string& id,
                            int load_path) {
        SCOPED_TRACE(load_path);
        GameSession reader;
        Configure(reader);
        if (failure == LoadFailure::Scheduler)
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
        if (load_path == 2) {
            EXPECT_FALSE(reader.LoadWorldFrom(id, root));
        } else if (load_path == 1) {
            EXPECT_FALSE(reader.LoadWorld(id));
        } else {
            EXPECT_FALSE(reader.LoadWorldStateFrom(save));
        }
        EXPECT_FALSE(reader.GetWorldOpenError().empty());
        if (failure == LoadFailure::Scheduler) {
            EXPECT_NE(reader.GetWorldOpenError().find("explicit override"), std::string::npos);
        } else if (failure == LoadFailure::PartialLedger || failure == LoadFailure::EmptyLedger) {
            EXPECT_EQ(reader.GetWorldOpenError(), world::ActiveRegionLedger::kCorruptMessage);
        } else if (failure == LoadFailure::Sdf) {
            EXPECT_EQ(reader.GetWorldOpenError(), "corrupt world chunk SDF lattice");
        } else if (failure == LoadFailure::Cursor) {
            EXPECT_NE(reader.GetWorldOpenError().find("water simulation cursor"),
                      std::string::npos);
        } else if (failure == LoadFailure::ActiveRegionsRequired) {
            EXPECT_EQ(reader.GetWorldOpenError(),
                      "Incompatible configuration: this world requires sim.active_regions.");
        }
        EXPECT_EQ(reader.GetRenderCalendar(), calendar);
        EXPECT_EQ(reader.GetWorldClock().canonical_bytes(), clock);
        EXPECT_EQ(reader.GetActiveRegionLedger().canonical_bytes(), ledger);
        EXPECT_EQ(reader.GetMetadata().worldId, previous_id);
        EXPECT_EQ(reader.GetWorldSystem(), previous_system);
        EXPECT_EQ(reader.GetWorldSystem()->GetWaterSimWindowCursor(), 19u);
        const auto retained = reader.GetWorldSystem()->snapshot_streamed_chunks();
        // Catalog refusals precede the state readers and retain the old world.
        // SDF and scheduler checks run after preflight; all snapshot refusals clear.
        if (load_path == 0 || failure == LoadFailure::Sdf || failure == LoadFailure::Scheduler) {
            EXPECT_TRUE(retained.empty());
            const auto refused_export = root / "refused-export";
            EXPECT_FALSE(reader.SaveWorldStateTo(refused_export));
            EXPECT_FALSE(fs::exists(refused_export));
        } else {
            ASSERT_EQ(retained.size(), 1u);
            EXPECT_EQ(retained.front(), retained_chunk);
        }
    }

    void CheckRefusedLoad(LoadFailure failure,
                          const fs::path& save,
                          const std::string& id,
                          const nlohmann::json& metadata) {
        SCOPED_TRACE(static_cast<int>(failure));
        ASSERT_TRUE(
            WorldSaveService::write_file_durably(save / "world_info.json", metadata.dump()));
        WorldStreamingState state;
        auto chunk = state.get_or_create_chunk(IVec3(0, 0, 0));
        chunk->set_state(ChunkState::Idle);
        chunk->sdf_data =
            failure == LoadFailure::Sdf ? std::vector<float>{1.0f} : std::vector<float>{};
        ASSERT_TRUE(WorldSaveService{}.save_world(state, save));
        if (failure == LoadFailure::PartialLedger || failure == LoadFailure::EmptyLedger ||
            failure == LoadFailure::Scheduler || failure == LoadFailure::ActiveRegionsRequired) {
            auto clock_metadata = metadata;
            clock_metadata.erase("renderCalendar");
            world::WorldClock(0, world::WorldCalendar::Legacy()).write_metadata(clock_metadata);
            ASSERT_TRUE(WorldSaveService::write_file_durably(save / "world_info.json",
                                                             clock_metadata.dump()));
        }
        const auto ledger_path = WorldSaveService::active_regions_path(save);
        fs::remove(ledger_path);
        if (failure == LoadFailure::PartialLedger || failure == LoadFailure::EmptyLedger) {
            ASSERT_TRUE(WorldSaveService::write_file_durably(
                ledger_path, failure == LoadFailure::PartialLedger ? "partial ledger" : ""));
        }
        if (failure == LoadFailure::Scheduler) {
            world::ActiveRegionLedger persisted(world::RegionSchedulerConfig{10, 3, 2});
            persisted.set_local_anchor(Vec3(8, 18, 8));
            ASSERT_TRUE(WorldSaveService::write_file_durably(ledger_path, persisted.encode()));
        }
        if (failure == LoadFailure::Chunk) {
            const auto region = WorldSaveService::region_file_path(save, 0, 0);
            ASSERT_TRUE(WorldSaveService::write_file_durably(region, "partial chunk"));
        }
        if (failure == LoadFailure::PartialMetadata) {
            ASSERT_TRUE(WorldSaveService::write_file_durably(save / "world_info.json", "{"));
        } else if (failure == LoadFailure::UnreadableMetadata) {
            ASSERT_TRUE(fs::remove(save / "world_info.json"));
            ASSERT_TRUE(fs::create_directory(save / "world_info.json"));
        } else if (failure == LoadFailure::Cursor) {
            auto invalid = metadata;
            invalid["waterSimCursor"] = -1;
            ASSERT_TRUE(
                WorldSaveService::write_file_durably(save / "world_info.json", invalid.dump()));
        }
        if (failure == LoadFailure::Sdf) {
            GameSession unopened;
            Configure(unopened);
            const auto calendar = unopened.GetRenderCalendar();
            EXPECT_FALSE(unopened.LoadWorld(id));
            EXPECT_EQ(unopened.GetWorldSystem(), nullptr);
            EXPECT_EQ(unopened.GetRenderCalendar(), calendar);
            EXPECT_EQ(unopened.GetWorldOpenError(), "corrupt world chunk SDF lattice");
        }
        for (const int load_path : {0, 1, 2}) {
            ASSERT_NO_FATAL_FAILURE(CheckRetainedWorld(failure, save, id, load_path));
        }
        // Restore the malformed container before preparing the next failure.
        fs::remove_all(WorldSaveService::region_directory(save));
        fs::remove(WorldSaveService::world_manifest_path(save));
        fs::remove(ledger_path);
        fs::remove_all(save / "world_info.json");
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

TEST_P(WorldStateCommit, IncrementalZeroCursorPreservesAbsentMetadataAndUpdatesExistingMetadata) {
    for (const bool canonical : {false, true}) {
        for (const bool dirty : {false, true}) {
            ASSERT_NO_FATAL_FAILURE(CheckIncrementalZeroCursor(canonical, dirty));
        }
    }
}

TEST_P(WorldStateCommit, RefusedLoadsClearResidentChunksWithoutPublishingStagedMembers) {
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
    std::vector<LoadFailure> failures{LoadFailure::Sdf,
                                      LoadFailure::Chunk,
                                      LoadFailure::PartialMetadata,
                                      LoadFailure::UnreadableMetadata,
                                      LoadFailure::Cursor};
    if (GetParam()) {
        failures.insert(
            failures.end(),
            {LoadFailure::PartialLedger, LoadFailure::EmptyLedger, LoadFailure::Scheduler});
    } else {
        failures.push_back(LoadFailure::ActiveRegionsRequired);
    }
    for (const auto failure : failures) {
        ASSERT_NO_FATAL_FAILURE(CheckRefusedLoad(failure, save, id, metadata));
    }
}

INSTANTIATE_TEST_SUITE_P(ActiveRegionsEnabledAndDisabled, WorldStateCommit, testing::Bool());
} // namespace
