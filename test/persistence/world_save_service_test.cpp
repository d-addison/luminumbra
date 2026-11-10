#include "../fixtures/TerrainFixtureParams.h"
#include "gtest/gtest.h"

#include "../support/CalendarMetadataCases.h"
#include "../support/PersistenceFileOps.h"
#include "../support/UniqueTempPath.h"
#include "../support/WorldgenTestData.h"

#include "core/FilesystemPath.h"
#include "core/JobSystem.h"
#include "core/Log.h"
#include "core/LogDispatcherSink.h"
#include "fields/FieldPageState.h"
#include "luminumbra/core/Types.h"
#include "persistence/CreatedWorldArtifacts.h"
#include "persistence/PlantPersistence.h"
#include "persistence/SaveTransaction.h"
#include "persistence/SavedInputFile.h"
#include "persistence/WorldPersistenceRoundtrip.h"
#include "persistence/WorldSaveService.h"
#include "systems/SHIELD_WorldSystem.h"
#include "world/ActiveRegionLedger.h"
#include "world/Chunk.h"
#include "world/GameSession.h"
#include "world/GroundObjectStore.h"
#include "world/MarchingCubes.h"
#include "world/WorldClock.h"
#include "world/WorldStreamingState.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <span>
#include <spdlog/sinks/ostream_sink.h>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

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

class TempSaveDir {
public:
    explicit TempSaveDir(const std::string& tag)
        : m_path(MakeTempSaveDir(tag)) {}
    ~TempSaveDir() {
        std::error_code remove_error;
        std::filesystem::remove_all(m_path, remove_error);
    }

    const std::filesystem::path& path() const {
        return m_path;
    }

private:
    std::filesystem::path m_path;
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

namespace {
std::string ReadFileBytes(const std::filesystem::path& path) {
    const std::ifstream input(path, std::ios::binary);
    std::ostringstream bytes;
    bytes << input.rdbuf();
    return bytes.str();
}

class DirectoryFlushReset {
public:
    ~DirectoryFlushReset() {
        WorldSaveService::set_directory_flush_failure_for_testing(-1);
        WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
    }
};
} // namespace

TEST(WorldSaveService, NoReplacePublicationPreservesOccupiedEntriesAndRetries) {
    const TempSaveDir save_dir("no_replace");
    const auto destination = save_dir.path() / std::filesystem::path(u8"preset-\u4e16\u754c.json");
    const std::string original("exact\0bytes\r\n", 13);
    const DirectoryFlushReset reset;
    WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    EXPECT_FALSE(WorldSaveService::create_file_durably(destination, "interrupted"));
    WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
    EXPECT_FALSE(std::filesystem::exists(destination));
    ASSERT_TRUE(WorldSaveService::create_file_durably(destination, original));
    for (int attempt = 0; attempt < 2; ++attempt) {
        EXPECT_FALSE(WorldSaveService::create_file_durably(destination, "replacement"));
        EXPECT_EQ(ReadFileBytes(destination), original);
    }
    EXPECT_FALSE(WorldSaveService::create_file_durably(destination / "child", "refused"));
    EXPECT_EQ(ReadFileBytes(destination), original);
    ASSERT_TRUE(WorldSaveService::remove_file_and_staging(destination));
    ASSERT_TRUE(WorldSaveService::create_file_durably(destination, "retry"));
    EXPECT_EQ(ReadFileBytes(destination), "retry");
    const auto blocked = save_dir.path() / "blocked";
    std::filesystem::create_directory(blocked);
    EXPECT_FALSE(WorldSaveService::create_file_durably(blocked, "refused"));
    EXPECT_TRUE(std::filesystem::is_directory(blocked));
#ifndef _WIN32
    // Native Windows coverage needs no symlink privilege; the POSIX branch also
    // checks the occupied entry for a dangling link, which exists() would miss.
    const auto dangling = save_dir.path() / "dangling";
    std::filesystem::create_symlink(save_dir.path() / "missing", dangling);
    EXPECT_FALSE(WorldSaveService::create_file_durably(dangling, "refused"));
    EXPECT_TRUE(std::filesystem::is_symlink(dangling));
#endif
    for (const auto& entry : std::filesystem::directory_iterator(save_dir.path()))
        EXPECT_EQ(
            Luminumbra::Filesystem::GenericUtf8PathString(entry.path().filename()).find(".tmp."),
            std::string::npos);
}

TEST(WorldSaveService, DurablePublicationFailuresExplainRefusals) {
    const TempSaveDir save_dir("publication_diagnostics");
    const auto source = save_dir.path() / "source";
    const auto destination = save_dir.path() / "destination";
    const DirectoryFlushReset reset;
    std::vector<std::string> errors;
    WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    EXPECT_FALSE(WorldSaveService::create_file_durably(destination, "new", &errors));
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors.front(), "interrupted before exclusive file publication");
    EXPECT_FALSE(std::filesystem::exists(destination));
    ASSERT_TRUE(WorldSaveService::write_file_durably(source, "original"));
    errors.clear();
    WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    EXPECT_FALSE(
        WorldSaveService::rename_file_durably(source, destination, "original", "new", &errors));
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors.front(), "interrupted before rename publication");
    EXPECT_EQ(ReadFileBytes(source), "original");
    EXPECT_FALSE(std::filesystem::exists(destination));
    errors.clear();
    EXPECT_FALSE(WorldSaveService::rename_file_durably(
        source, source / "child" / "file", "original", "new", &errors));
    ASSERT_EQ(errors.size(), 1u);
    // Libraries report a non-directory ancestor either during leaf inspection
    // or during directory creation. Both paths must explain the refusal.
    EXPECT_TRUE(errors.front() == "rename destination is occupied or cannot be inspected" ||
                errors.front().starts_with("cannot create rename publication directory:"));
    EXPECT_EQ(ReadFileBytes(source), "original");
    errors.clear();
    WorldSaveService::PreparedWorldWrite prepared;
    EXPECT_FALSE(prepared.publish(&errors));
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors.front(), "world write has not been prepared or was already consumed");
}

TEST(WorldSaveService, ConcurrentNoReplacePublicationHasExactlyOneWinner) {
    const TempSaveDir save_dir("no_replace_race");
    const auto destination = save_dir.path() / "winner";
    std::barrier<> ready{3};
    std::array<bool, 2> saved{};
    const std::array<std::string, 2> payloads{"first exact bytes", "second exact bytes"};
    auto publish = [&](std::size_t index) {
        ready.arrive_and_wait();
        saved[index] = WorldSaveService::create_file_durably(destination, payloads[index]);
    };
    std::jthread first(publish, 0u);
    std::jthread second(publish, 1u);
    ready.arrive_and_wait();
    first.join();
    second.join();
    ASSERT_NE(saved[0], saved[1]);
    EXPECT_EQ(ReadFileBytes(destination), saved[0] ? payloads[0] : payloads[1]);
    for (const auto& entry : std::filesystem::directory_iterator(save_dir.path()))
        EXPECT_EQ(entry.path(), destination);
}

TEST(WorldSaveService, CheckedRenameRefusesChangedSourceBeforePublishing) {
    const TempSaveDir save_dir("stale_rename");
    const auto source = save_dir.path() / "source";
    const auto destination = save_dir.path() / "destination";
    ASSERT_TRUE(WorldSaveService::write_file_durably(source, "newer source"));
    EXPECT_FALSE(WorldSaveService::rename_file_durably(source, destination, "old source", "new"));
    EXPECT_EQ(ReadFileBytes(source), "newer source");
    EXPECT_FALSE(std::filesystem::exists(destination));
    ASSERT_TRUE(WorldSaveService::rename_file_durably(source, destination, "newer source", "new"));
    EXPECT_FALSE(std::filesystem::exists(source));
    EXPECT_EQ(ReadFileBytes(destination), "new");
}

TEST(WorldSaveService, CheckedRenameRetiresHardLinkAliasAndKeepsPathAlias) {
    const TempSaveDir save_dir("rename_entry_aliases");
    const auto source = save_dir.path() / "source.json";
    const auto destination = save_dir.path() / "destination.json";
    ASSERT_TRUE(WorldSaveService::write_file_durably(source, "original"));
    std::filesystem::create_hard_link(source, destination);
    ASSERT_TRUE(WorldSaveService::rename_file_durably(source, destination, "original", "new"));
    EXPECT_FALSE(std::filesystem::exists(source));
    EXPECT_EQ(ReadFileBytes(destination), "new");
    const auto path_alias = save_dir.path() / "." / destination.filename();
    ASSERT_TRUE(WorldSaveService::rename_file_durably(destination, path_alias, "new", "updated"));
    EXPECT_EQ(ReadFileBytes(destination), "updated");
    for (const auto& entry : std::filesystem::directory_iterator(save_dir.path()))
        EXPECT_EQ(entry.path(), destination);
}

TEST(WorldSaveService, CheckedRenameHandlesCaseOnlyDestination) {
    const TempSaveDir save_dir("rename_case");
    const auto source = save_dir.path() / "preset.json";
    const auto destination = save_dir.path() / "PRESET.JSON";
    ASSERT_TRUE(WorldSaveService::write_file_durably(source, "original"));
    ASSERT_TRUE(WorldSaveService::rename_file_durably(source, destination, "original", "new"));
    EXPECT_EQ(ReadFileBytes(destination), "new");
    std::size_t entries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(save_dir.path())) {
        EXPECT_EQ(ReadFileBytes(entry.path()), "new");
        ++entries;
    }
    EXPECT_EQ(entries, 1u);
}

#ifndef _WIN32
TEST(WorldSaveService, CheckedRenameRetiresLeafSymlinkToDestination) {
    const TempSaveDir save_dir("rename_leaf_to_destination");
    const auto source = save_dir.path() / "source.json";
    const auto destination = save_dir.path() / "destination.json";
    ASSERT_TRUE(WorldSaveService::write_file_durably(destination, "original"));
    std::filesystem::create_symlink(destination.filename(), source);
    const DirectoryFlushReset reset;
    WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    EXPECT_FALSE(WorldSaveService::rename_file_durably(source, destination, "original", "new"));
    EXPECT_TRUE(std::filesystem::is_symlink(std::filesystem::symlink_status(source)));
    EXPECT_EQ(ReadFileBytes(destination), "original");
    ASSERT_TRUE(WorldSaveService::rename_file_durably(source, destination, "original", "new"));
    EXPECT_EQ(std::filesystem::symlink_status(source).type(),
              std::filesystem::file_type::not_found);
    EXPECT_EQ(ReadFileBytes(destination), "new");
    for (const auto& entry : std::filesystem::directory_iterator(save_dir.path()))
        EXPECT_EQ(entry.path(), destination);
}

TEST(WorldSaveService, CheckedRenameLeafSymlinkPreservesExternalTargetAndOccupiedDestination) {
    const TempSaveDir save_dir("rename_leaf_external");
    const auto external = save_dir.path() / "external.json";
    const auto source = save_dir.path() / "source.json";
    const auto destination = save_dir.path() / "destination.json";
    ASSERT_TRUE(WorldSaveService::write_file_durably(external, "original"));
    std::filesystem::create_symlink(external.filename(), source);
    ASSERT_TRUE(WorldSaveService::write_file_durably(destination, "occupied"));
    EXPECT_FALSE(WorldSaveService::rename_file_durably(source, destination, "original", "new"));
    EXPECT_EQ(ReadFileBytes(destination), "occupied");
    EXPECT_EQ(ReadFileBytes(external), "original");
    EXPECT_TRUE(std::filesystem::is_symlink(std::filesystem::symlink_status(source)));
    ASSERT_TRUE(WorldSaveService::remove_file_and_staging(destination));
    ASSERT_TRUE(WorldSaveService::rename_file_durably(source, destination, "original", "new"));
    EXPECT_EQ(std::filesystem::symlink_status(source).type(),
              std::filesystem::file_type::not_found);
    EXPECT_EQ(ReadFileBytes(destination), "new");
    EXPECT_EQ(ReadFileBytes(external), "original");
}

TEST(WorldSaveService, CheckedRenameRefusesDanglingSourceAndDestination) {
    const TempSaveDir save_dir("rename_dangling");
    const auto source = save_dir.path() / "source.json";
    const auto destination = save_dir.path() / "destination.json";
    const auto missing = save_dir.path() / "missing.json";
    std::filesystem::create_symlink(missing.filename(), source);
    EXPECT_FALSE(WorldSaveService::rename_file_durably(source, destination, "", "new"));
    EXPECT_TRUE(std::filesystem::is_symlink(std::filesystem::symlink_status(source)));
    EXPECT_FALSE(std::filesystem::exists(destination));
    ASSERT_TRUE(WorldSaveService::remove_file_and_staging(source));
    ASSERT_TRUE(WorldSaveService::write_file_durably(source, "original"));
    std::filesystem::create_symlink(missing.filename(), destination);
    EXPECT_FALSE(WorldSaveService::rename_file_durably(source, destination, "original", "new"));
    EXPECT_EQ(ReadFileBytes(source), "original");
    EXPECT_TRUE(std::filesystem::is_symlink(std::filesystem::symlink_status(destination)));
    EXPECT_FALSE(std::filesystem::exists(missing));
}

TEST(WorldSaveService, CheckedRenameKeepsParentSymlinkAliasAndRetiresLeafDestinationAlias) {
    const TempSaveDir save_dir("rename_parent_alias");
    const auto parent = save_dir.path() / "actual";
    const auto parent_alias = save_dir.path() / "alias";
    std::filesystem::create_directory(parent);
    std::filesystem::create_directory_symlink(parent.filename(), parent_alias);
    const auto source = parent / "source.json";
    const auto destination = parent_alias / source.filename();
    ASSERT_TRUE(WorldSaveService::write_file_durably(source, "original"));
    ASSERT_TRUE(WorldSaveService::rename_file_durably(source, destination, "original", "new"));
    EXPECT_EQ(ReadFileBytes(source), "new");
    EXPECT_TRUE(std::filesystem::is_symlink(std::filesystem::symlink_status(parent_alias)));
    const auto leaf_alias = parent / "leaf.json";
    std::filesystem::create_symlink(source.filename(), leaf_alias);
    const DirectoryFlushReset reset;
    WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    EXPECT_FALSE(WorldSaveService::rename_file_durably(source, leaf_alias, "new", "updated"));
    EXPECT_TRUE(std::filesystem::is_symlink(std::filesystem::symlink_status(leaf_alias)));
    EXPECT_EQ(ReadFileBytes(source), "new");
    ASSERT_TRUE(WorldSaveService::rename_file_durably(source, leaf_alias, "new", "updated"));
    EXPECT_FALSE(std::filesystem::exists(source));
    EXPECT_TRUE(std::filesystem::is_regular_file(std::filesystem::symlink_status(leaf_alias)));
    EXPECT_EQ(ReadFileBytes(leaf_alias), "updated");
}

TEST(WorldSaveService, FirstSnapshotRefusesUnflushedAncestorAndRepeatsOnRetry) {
    const TempSaveDir save_dir("snapshot_ancestor");
    const WorldSaveService service;
    WorldStreamingState state;
    AddFixtureChunk(state, IVec3(0, 0, 0), ChunkState::Ready, 1u);
    const DirectoryFlushReset reset;
    for (int attempt = 0; attempt < 2; ++attempt) {
        WorldSaveService::set_directory_flush_failure_for_testing(0);
        std::vector<std::string> errors;
        EXPECT_FALSE(service.save_world(state, save_dir.path(), &errors));
        ASSERT_EQ(errors.size(), 1u);
        EXPECT_EQ(errors.front(),
                  "directory flush failed (test seam): " + (save_dir.path() / "chunks").string());
        EXPECT_FALSE(
            std::filesystem::exists(WorldSaveService::region_file_path(save_dir.path(), 0, 0)));
        EXPECT_FALSE(
            std::filesystem::exists(WorldSaveService::world_manifest_path(save_dir.path())));
    }
    ASSERT_TRUE(service.save_world(state, save_dir.path()));
    WorldStreamingState loaded;
    std::vector<std::string> errors;
    ASSERT_TRUE(service.load_world(loaded, save_dir.path(), errors));
    EXPECT_EQ(service.world_hash(state), service.world_hash(loaded));
}

class RacingStagingAlias {
public:
    explicit RacingStagingAlias(const std::filesystem::path& destination) {
        Target() = destination;
        Injected() = false;
        Luminumbra::Persistence::detail::set_file_publication_hook_for_testing(&Publish);
    }
    ~RacingStagingAlias() {
        Reset();
    }
    static void Reset() {
        Luminumbra::Persistence::detail::set_file_publication_hook_for_testing(nullptr);
    }
    static bool& Injected() {
        static bool injected = false;
        return injected;
    }

private:
    static std::filesystem::path& Target() {
        static std::filesystem::path target;
        return target;
    }
    static bool Publish(const std::filesystem::path& destination, bool removing) {
        if (removing || destination != Target())
            return true;
        std::filesystem::path staged;
        std::size_t inspected = 0;
        // Private fixture: source, staging file and at most one publication.
        for (const auto& entry : std::filesystem::directory_iterator(destination.parent_path())) {
            if (++inspected > 3) {
                ADD_FAILURE() << "Unexpected publication fixture entry";
                return false;
            }
            const auto prefix =
                destination.filename().native() + std::filesystem::path(".tmp.").native();
            if (entry.path().filename().native().starts_with(prefix)) {
                EXPECT_TRUE(staged.empty());
                staged = entry.path();
            }
        }
        EXPECT_FALSE(staged.empty());
        if (staged.empty())
            return false;
        std::filesystem::create_hard_link(staged, destination);
        Injected() = true;
        return true;
    }
};

TEST(WorldSaveService, RacingStagingAliasNeverTransfersPublicationOwnership) {
    for (const bool rename_source : {false, true}) {
        SCOPED_TRACE(rename_source);
        const TempSaveDir save_dir("racing_staging_alias");
        const auto source = save_dir.path() / "source";
        const auto destination = save_dir.path() / "destination";
        ASSERT_TRUE(WorldSaveService::create_file_durably(source, "original bytes"));
        const RacingStagingAlias race(destination);
        if (rename_source)
            EXPECT_FALSE(WorldSaveService::rename_file_durably(
                source, destination, "original bytes", "racing bytes"));
        else
            EXPECT_FALSE(WorldSaveService::create_file_durably(destination, "racing bytes"));
        RacingStagingAlias::Reset();
        ASSERT_TRUE(RacingStagingAlias::Injected());
        EXPECT_EQ(ReadFileBytes(source), "original bytes");
        EXPECT_EQ(ReadFileBytes(destination), "racing bytes");
        ASSERT_TRUE(std::filesystem::remove(destination));
        ASSERT_TRUE(WorldSaveService::create_file_durably(destination, "retry bytes"));
        EXPECT_EQ(ReadFileBytes(destination), "retry bytes");
    }
}

TEST(WorldSaveService, NewPublicationFlushFailureRollsBackAndCanRetry) {
    const TempSaveDir save_dir("publication_flush");
    const auto destination = save_dir.path() / "destination";
    const DirectoryFlushReset reset;
    int ancestors = 0;
    for (auto ancestor = destination.parent_path();
         !ancestor.empty() && ancestor != ancestor.root_path();
         ancestor = ancestor.parent_path())
        ++ancestors;
    WorldSaveService::set_directory_flush_failure_for_testing(ancestors);
    EXPECT_FALSE(WorldSaveService::create_file_durably(destination, "complete bytes"));
    EXPECT_FALSE(std::filesystem::exists(destination));
    EXPECT_TRUE(WorldSaveService::create_file_durably(destination, "retry"));
    EXPECT_EQ(ReadFileBytes(destination), "retry");
}

TEST(WorldSaveService, SourceRetirementFlushFailureKeepsPublishedContentAndCanReflush) {
    const TempSaveDir save_dir("retirement_flush");
    const auto source = save_dir.path() / "source";
    const auto destination = save_dir.path() / "destination";
    ASSERT_TRUE(WorldSaveService::create_file_durably(source, "original bytes"));
    const DirectoryFlushReset reset;
    int before_source_flush = 1; // The destination's publication-directory flush.
    for (auto ancestor = destination.parent_path();
         !ancestor.empty() && ancestor != ancestor.root_path();
         ancestor = ancestor.parent_path())
        ++before_source_flush;
    WorldSaveService::set_directory_flush_failure_for_testing(before_source_flush);
    EXPECT_FALSE(
        WorldSaveService::rename_file_durably(source, destination, "original bytes", "new"));
    EXPECT_FALSE(std::filesystem::exists(source));
    EXPECT_EQ(ReadFileBytes(destination), "new");
    // Source unlink has completed; repeat its parent flush even on an absent-file retry.
    WorldSaveService::set_directory_flush_failure_for_testing(0);
    EXPECT_FALSE(WorldSaveService::remove_file_and_staging(source));
    EXPECT_TRUE(WorldSaveService::remove_file_and_staging(source));
    EXPECT_EQ(ReadFileBytes(destination), "new");
}
#endif

TEST(WorldSaveService, SaveThenLoadRoundTripPreservesWorldHash) {
    const TempSaveDir save_dir("roundtrip");
    const WorldSaveService service;

    WorldStreamingState original;
    PopulateFixtureWorld(original);
    const std::string original_hash = service.world_hash(original);
    ASSERT_FALSE(original_hash.empty());

    std::vector<std::string> save_errors;
    ASSERT_TRUE(service.save_world(original, save_dir.path(), &save_errors));
    EXPECT_TRUE(save_errors.empty());
    // Persistence v2: the writer emits the LMR1 region container,
    // never the legacy v1 single snapshot.
    EXPECT_FALSE(std::filesystem::exists(WorldSaveService::world_state_path(save_dir.path())));
    EXPECT_TRUE(std::filesystem::exists(WorldSaveService::world_manifest_path(save_dir.path())));
    EXPECT_TRUE(std::filesystem::exists(WorldSaveService::region_file_path(save_dir.path(), 0, 0)));

    WorldStreamingState restored;
    std::vector<std::string> load_errors;
    ASSERT_TRUE(service.load_world(restored, save_dir.path(), load_errors));
    EXPECT_TRUE(load_errors.empty());

    EXPECT_EQ(restored.size(), original.size());
    EXPECT_EQ(service.world_hash(restored), original_hash);
}

TEST(WorldSaveService, AbandonedRegionAndManifestStagingAllowsLoadAndIsReclaimedOnRetry) {
    const TempSaveDir save_dir("abandoned_staging");
    const WorldSaveService service;
    WorldStreamingState state;
    PopulateFixtureWorld(state);
    ASSERT_TRUE(service.save_world(state, save_dir.path()));
    const auto expected_hash = service.world_hash(state);
    const auto region = WorldSaveService::region_file_path(save_dir.path(), 0, 0);
    const auto manifest = WorldSaveService::world_manifest_path(save_dir.path());
    std::vector<std::filesystem::path> abandoned;
    for (const auto& destination : {region, manifest}) {
        const auto staging = save_dir.path() / (destination.filename().string() + ".tmp.abandoned");
        ASSERT_TRUE(WorldSaveService::write_file_durably(staging, "partial bytes"));
        abandoned.push_back(staging);
    }
    const auto retained_directory = save_dir.path() / "r.0.0.lmr.tmp.directory";
    std::filesystem::create_directory(retained_directory);
    WorldStreamingState restored;
    std::vector<std::string> errors;
    ASSERT_TRUE(service.load_world(restored, save_dir.path(), errors));
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(service.world_hash(restored), expected_hash);
    ASSERT_TRUE(service.save_world(state, save_dir.path()));
    for (const auto& staging : abandoned)
        EXPECT_FALSE(std::filesystem::exists(staging));
    EXPECT_TRUE(std::filesystem::is_directory(retained_directory));
    ASSERT_TRUE(service.load_world(restored, save_dir.path(), errors));
    EXPECT_EQ(service.world_hash(restored), expected_hash);
}

TEST(WorldSaveService, StandaloneContainerReclaimsAdjacentStagingAndPreservesRecords) {
    const TempSaveDir save_dir("standalone_staging");
    const auto path = save_dir.path() / "standalone/records.lmr";
    const WorldSaveService::ContainerRecord original{
        .id = 7, .lod_level = 1, .flags = 0, .payload = "original"};
    ASSERT_TRUE(WorldSaveService::upsert_container_records(path, {original}));
    auto abandoned = path;
    abandoned += ".tmp.abandoned";
    ASSERT_TRUE(WorldSaveService::write_file_durably(abandoned, "partial bytes"));
    const WorldSaveService::ContainerRecord added{
        .id = 8, .lod_level = 1, .flags = 0, .payload = "added"};
    ASSERT_TRUE(WorldSaveService::upsert_container_records(path, {added}));
    EXPECT_FALSE(std::filesystem::exists(abandoned));
    std::vector<WorldSaveService::ContainerRecord> records;
    ASSERT_TRUE(WorldSaveService::read_container_records(path, records));
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].id, original.id);
    EXPECT_EQ(records[0].payload, original.payload);
    EXPECT_EQ(records[1].id, added.id);
    EXPECT_EQ(records[1].payload, added.payload);
}

TEST(WorldSaveService, ContainerStagingKeepsUnrecognizedRegionSpellingAdjacent) {
    const TempSaveDir save_dir("unrecognized_staging");
    const auto canonical = WorldSaveService::region_file_path(save_dir.path(), 0, 0);
    const auto path = canonical.parent_path() / "." / canonical.filename();
    const WorldSaveService::ContainerRecord original{
        .id = 7, .lod_level = 1, .flags = 0, .payload = "original"};
    ASSERT_TRUE(WorldSaveService::upsert_container_records(path, {original}));
    EXPECT_EQ(WorldSaveService::region_staging_path(path).native(), path.native());
    auto abandoned = path;
    abandoned += ".tmp.abandoned";
    ASSERT_TRUE(WorldSaveService::write_file_durably(abandoned, "partial bytes"));
    ASSERT_TRUE(WorldSaveService::upsert_container_records(path, {original}));
    EXPECT_FALSE(std::filesystem::exists(abandoned));
    std::vector<WorldSaveService::ContainerRecord> records;
    ASSERT_TRUE(WorldSaveService::read_container_records(canonical, records));
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records.front().payload, original.payload);
}

TEST(WorldSaveService, RegionStagingPreservesRootsAndStandaloneSpelling) {
    namespace paths = Luminumbra::Filesystem;
    const auto check_root = [](const std::filesystem::path& root, const std::string& expected) {
        const auto region = root / "chunks/region/r.0.0.lmr";
        const auto staging = WorldSaveService::region_staging_path(region);
        EXPECT_EQ(staging.native(), (root / region.filename()).native());
        EXPECT_EQ(paths::GenericUtf8PathString(staging), expected + "/r.0.0.lmr");
        for (const auto& suffix : {std::filesystem::path("standalone/./records.lmr"),
                                   std::filesystem::path("standalone/child/../records.lmr"),
                                   std::filesystem::path("chunks/region/./r.0.0.lmr"),
                                   std::filesystem::path("chunks/./region/r.0.0.lmr"),
                                   std::filesystem::path("chunks/unused/../region/r.0.0.lmr"),
                                   std::filesystem::path("world-manifest.json")}) {
            const auto destination = root / suffix;
            EXPECT_EQ(WorldSaveService::region_staging_path(destination).native(),
                      destination.native());
        }
    };
    check_root("save", "save");
    EXPECT_EQ(WorldSaveService::region_staging_path("records.lmr").native(),
              std::filesystem::path("records.lmr").native());
#if defined(_WIN32) && !defined(__CYGWIN__)
    // Path-only checks need neither a live share nor symlink privileges. They
    // exercise the same staging calculation used by both durable writers.
    check_root("C:/save", "C:/save");
    check_root("//server/share/save", "//server/share/save");
    check_root(LR"(\\server\share\save)", "//server/share/save");
    check_root(LR"(//server/share\save)", "//server/share/save");
    check_root("//server/share", "//server/share");
    check_root(L"\\\\server\\share\\caf\u00e9", "//server/share/caf\xc3\xa9");
#else
    check_root("/save", "/save");
#endif
}

TEST(WorldSaveService, RegionStagingRetainsSymlinkParentTraversalWithoutFilesystemAccess) {
    // Missing paths also exercise this contract on Windows without symlink privilege.
    // Four spellings bound the pass; none is opened or resolved.
    for (const auto& root : {std::filesystem::path("missing/link/../save"),
                             std::filesystem::path("missing/link/.."),
                             std::filesystem::path("missing/link/../save/./"),
                             std::filesystem::path("missing/link/../../save")}) {
        for (const auto* member : {"r.0.0.lmr", "world-manifest.json"}) {
            const auto destination = WorldSaveService::region_directory(root) / member;
            // Ignore separator spelling while retaining every "." and ".." component.
            EXPECT_EQ(WorldSaveService::region_staging_path(destination).generic_string(),
                      (root / member).generic_string());
        }
    }
}

#if !defined(_WIN32)
TEST(WorldSaveService, SymlinkedRegionStagesInsideSaveRootWithReadOnlyTargetAncestors) {
    const TempSaveDir fixture("symlink_region_staging");
    const auto save_root = fixture.path() / "save";
    const auto storage = save_root / "storage";
    const auto regions = storage / "regions";
    const auto target = regions / "worldA";
    std::filesystem::create_directories(target);
    std::filesystem::create_directories(save_root / "chunks");
    std::filesystem::create_directory_symlink(target,
                                              WorldSaveService::region_directory(save_root));

    class ReadOnlyDirectory {
    public:
        explicit ReadOnlyDirectory(const std::filesystem::path& path)
            : m_path(path)
            , m_permissions(std::filesystem::status(path).permissions()) {
            std::filesystem::permissions(
                m_path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
        }
        ~ReadOnlyDirectory() {
            std::error_code error;
            std::filesystem::permissions(m_path, m_permissions, error);
        }

    private:
        const std::filesystem::path m_path;
        const std::filesystem::perms m_permissions;
    };
    // Two ancestors bound this pass. Old timestamps detect even transient temp
    // creation when elevated privileges bypass the read-only permissions.
    const auto old_time = std::filesystem::last_write_time(storage) - std::chrono::hours(24);
    std::filesystem::last_write_time(storage, old_time);
    std::filesystem::last_write_time(regions, old_time);
    const auto storage_time = std::filesystem::last_write_time(storage);
    const auto regions_time = std::filesystem::last_write_time(regions);
    const ReadOnlyDirectory protect_storage(storage);
    const ReadOnlyDirectory protect_regions(regions);

    const auto region = WorldSaveService::region_file_path(save_root, 0, 0);
    const auto manifest = WorldSaveService::world_manifest_path(save_root);
    for (const auto& destination : {region, manifest}) {
        EXPECT_EQ(WorldSaveService::region_staging_path(destination),
                  save_root / destination.filename());
        const auto abandoned = save_root / (destination.filename().string() + ".tmp.abandoned");
        ASSERT_TRUE(WorldSaveService::write_file_durably(abandoned, "partial"));
    }
    const WorldSaveService service;
    WorldStreamingState state;
    PopulateFixtureWorld(state);
    ASSERT_TRUE(service.save_world(state, save_root));
    const auto original_hash = service.world_hash(state);
    EXPECT_TRUE(std::filesystem::is_regular_file(target / region.filename()));
    EXPECT_TRUE(std::filesystem::is_regular_file(target / manifest.filename()));
    WorldStreamingState loaded;
    std::vector<std::string> errors;
    ASSERT_TRUE(service.load_world(loaded, save_root, errors));
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(service.world_hash(loaded), original_hash);

    AddFixtureChunk(state, IVec3(0, 0, 0), ChunkState::Ready, 9u);
    const auto changed_hash = service.world_hash(state);
    ASSERT_NE(changed_hash, original_hash);
    WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    const bool interrupted = service.save_world(state, save_root);
    WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
    EXPECT_FALSE(interrupted);
    ASSERT_TRUE(service.load_world(loaded, save_root, errors));
    EXPECT_EQ(service.world_hash(loaded), original_hash);
    ASSERT_TRUE(service.save_world(state, save_root));
    ASSERT_TRUE(service.load_world(loaded, save_root, errors));
    EXPECT_EQ(service.world_hash(loaded), changed_hash);
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(std::filesystem::last_write_time(storage), storage_time);
    EXPECT_EQ(std::filesystem::last_write_time(regions), regions_time);
    // The isolated fixture has three chunks, a manifest and five directories;
    // default iteration does not follow the region symlink.
    for (const auto& entry : std::filesystem::recursive_directory_iterator(fixture.path()))
        EXPECT_EQ(
            Luminumbra::Filesystem::GenericUtf8PathString(entry.path().filename()).find(".tmp."),
            std::string::npos);
}

TEST(WorldSaveService, SymlinkParentSaveReclaimsAndReplacesInTheResolvedDirectory) {
    const TempSaveDir fixture("symlink_parent_staging");
    const auto actual_root = fixture.path() / "actual";
    std::filesystem::create_directories(actual_root / "child");
    std::filesystem::create_directory_symlink(actual_root / "child", fixture.path() / "link");
    const auto save_root = fixture.path() / "link/../save";
    const auto actual_save = actual_root / "save";
    const auto wrong_save = fixture.path() / "save";
    std::filesystem::create_directories(wrong_save);
    const auto unrelated = wrong_save / "r.0.0.lmr.tmp.retained";
    ASSERT_TRUE(WorldSaveService::write_file_durably(unrelated, "unrelated"));
    WorldStreamingState state;
    // The interruption stops at the first region; keep only the region whose
    // abandoned staging is checked below, independent of other fixture regions.
    AddFixtureChunk(state, IVec3(0, 0, 0), ChunkState::Ready, 1u);
    const WorldSaveService service;
    ASSERT_TRUE(service.save_world(state, save_root));
    const auto original_hash = service.world_hash(state);
    const auto abandoned = actual_save / "r.0.0.lmr.tmp.abandoned";
    const auto abandoned_manifest = actual_save / "world-manifest.json.tmp.abandoned";
    ASSERT_TRUE(WorldSaveService::write_file_durably(abandoned, "partial"));
    ASSERT_TRUE(WorldSaveService::write_file_durably(abandoned_manifest, "{partial"));
    AddFixtureChunk(state, IVec3(0, 0, 0), ChunkState::Ready, 9u);
    const auto changed_hash = service.world_hash(state);
    ASSERT_NE(changed_hash, original_hash);
    WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    const bool interrupted = service.save_world(state, save_root);
    WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
    EXPECT_FALSE(interrupted);
    EXPECT_FALSE(std::filesystem::exists(abandoned));
    // Manifest reclamation follows region replacement and is reached on retry.
    EXPECT_TRUE(std::filesystem::exists(abandoned_manifest));
    EXPECT_TRUE(std::filesystem::exists(unrelated));
    WorldStreamingState loaded;
    std::vector<std::string> errors;
    ASSERT_TRUE(service.load_world(loaded, save_root, errors));
    EXPECT_EQ(service.world_hash(loaded), original_hash);
    ASSERT_TRUE(service.save_world(state, save_root));
    EXPECT_FALSE(std::filesystem::exists(abandoned_manifest));
    ASSERT_TRUE(service.load_world(loaded, actual_save, errors));
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(service.world_hash(loaded), changed_hash);
    EXPECT_FALSE(std::filesystem::exists(wrong_save / "chunks"));
    EXPECT_TRUE(std::filesystem::exists(unrelated));
}
#endif

TEST(WorldSaveService, DurableContainerStagingHandlesParentComponentsAndInterruptedRetry) {
    const TempSaveDir save_dir("staging_paths");
    class RestoreDirectory {
    public:
        RestoreDirectory() = default;
        ~RestoreDirectory() {
            std::error_code error;
            std::filesystem::current_path(m_previous, error);
        }

    private:
        const std::filesystem::path m_previous = std::filesystem::current_path();
    };
    const RestoreDirectory restore_directory;
    std::filesystem::current_path(save_dir.path());
    // Five destinations bound this pass; each contains just one raw record.
    for (const auto* relative : {"normal/chunks/region/r.0.0.lmr",
                                 "dot/chunks/region/./r.0.0.lmr",
                                 "parent/chunks/unused/../region/r.0.0.lmr",
                                 "raw/child/../records.lmr",
                                 "records.lmr"}) {
        const std::filesystem::path destination(relative);
        if (destination.has_parent_path())
            std::filesystem::create_directories(destination.parent_path());
        const auto staging = WorldSaveService::region_staging_path(destination);
        auto abandoned = staging;
        abandoned += ".tmp.abandoned";
        const WorldSaveService::ContainerRecord original{
            .id = 7, .lod_level = 1, .flags = 0, .payload = "original"};
        ASSERT_TRUE(WorldSaveService::upsert_container_records(destination, {original}));
        ASSERT_TRUE(WorldSaveService::write_file_durably(abandoned, "partial"));
        const WorldSaveService::ContainerRecord changed{
            .id = 7, .lod_level = 1, .flags = 0, .payload = "changed"};
        WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
        const bool interrupted = WorldSaveService::upsert_container_records(destination, {changed});
        WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
        ASSERT_FALSE(interrupted);
        EXPECT_FALSE(std::filesystem::exists(abandoned));
        std::vector<WorldSaveService::ContainerRecord> records;
        ASSERT_TRUE(WorldSaveService::read_container_records(destination, records));
        ASSERT_EQ(records.size(), 1u);
        EXPECT_EQ(records.front().payload, original.payload);
        ASSERT_TRUE(WorldSaveService::upsert_container_records(destination, {changed}));
        ASSERT_TRUE(WorldSaveService::read_container_records(destination, records));
        ASSERT_EQ(records.size(), 1u);
        EXPECT_EQ(records.front().payload, changed.payload);
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(save_dir.path()))
        EXPECT_EQ(
            Luminumbra::Filesystem::GenericUtf8PathString(entry.path().filename()).find(".tmp."),
            std::string::npos);
}

namespace {
struct OverlapObservation {
    std::barrier<> staged{2};
    std::barrier<> release{2};
};
OverlapObservation*& ActiveOverlapObservation() {
    static OverlapObservation* observation = nullptr;
    return observation;
}

bool HoldStagedLedger() {
    ActiveOverlapObservation()->staged.arrive_and_wait();
    ActiveOverlapObservation()->release.arrive_and_wait();
    return true;
}

} // namespace

TEST(WorldSaveService, OverlappingRegionReclaimWaitsUntilStagedOwnerCompletes) {
    enum class Operation : std::uint8_t {
        Region,
        Shared,
        Empty
    };
    // Three clients and two threads bound this pass. Each real save overlaps
    // the staged ledger owner and uses the same staging base.
    for (const auto operation : {Operation::Region, Operation::Shared, Operation::Empty}) {
        const TempSaveDir save_dir("overlap");
        Luminumbra::world::WorldClock clock;
        Luminumbra::world::ActiveRegionLedger ledger;
        ledger.set_local_anchor(Vec3(1, 2, 3));
        OverlapObservation observation;
        ActiveOverlapObservation() = &observation;
        WorldSaveService::set_before_active_regions_replace_for_testing(&HoldStagedLedger);
        bool ledger_saved = false;
        bool region_saved = false;
        std::atomic<bool> region_done{false};
        std::jthread ledger_writer([&] {
            ledger_saved = WorldSaveService::save_active_regions(ledger, clock, save_dir.path());
        });
        observation.staged.arrive_and_wait();
        // Raw containers may use any destination. This one shares the ledger's
        // staging base, so an unprotected region reclaimer deletes its ACTIVE temp.
        const auto region = save_dir.path() / "active-regions.arl";
        const WorldSaveService::ContainerRecord record{
            .id = 7, .lod_level = 1, .flags = 0, .payload = "overlapping region"};
        std::jthread region_writer([&] {
            switch (operation) {
                case Operation::Region:
                    region_saved = WorldSaveService::upsert_container_records(region, {record});
                    break;
                case Operation::Shared:
                    region_saved = WorldSaveService::write_file_durably(region, record.payload);
                    break;
                case Operation::Empty:
                    region_saved = WorldSaveService::remove_file_and_staging(region);
                    break;
            }
            region_done.store(true, std::memory_order_release);
        });
        // No timing assumptions: the first writer cannot leave the barrier until
        // the second is either demonstrably waiting for ownership or has completed.
        while (WorldSaveService::waiting_save_writers() == 0 &&
               !region_done.load(std::memory_order_acquire))
            std::this_thread::yield();
        EXPECT_FALSE(region_done.load(std::memory_order_acquire));
        EXPECT_EQ(WorldSaveService::waiting_save_writers(), 1u);
        observation.release.arrive_and_wait();
        ledger_writer.join();
        region_writer.join();
        WorldSaveService::set_before_active_regions_replace_for_testing(nullptr);
        ActiveOverlapObservation() = nullptr;
        EXPECT_TRUE(ledger_saved);
        EXPECT_TRUE(region_saved);
        EXPECT_EQ(WorldSaveService::waiting_save_writers(), 0u);
        Luminumbra::world::ActiveRegionLedger loaded;
        ASSERT_TRUE(WorldSaveService::load_active_regions(loaded, save_dir.path()));
        EXPECT_EQ(loaded.local_anchor(), ledger.local_anchor());
        if (operation == Operation::Region) {
            std::vector<WorldSaveService::ContainerRecord> records;
            ASSERT_TRUE(WorldSaveService::read_container_records(region, records));
            ASSERT_EQ(records.size(), 1u);
            EXPECT_EQ(records.front().payload, record.payload);
        } else if (operation == Operation::Shared) {
            const std::ifstream input(region, std::ios::binary);
            std::ostringstream bytes;
            bytes << input.rdbuf();
            EXPECT_EQ(bytes.str(), record.payload);
        } else {
            EXPECT_FALSE(std::filesystem::exists(region));
        }
        for (const auto& entry : std::filesystem::directory_iterator(save_dir.path()))
            EXPECT_EQ(Luminumbra::Filesystem::GenericUtf8PathString(entry.path().filename())
                          .find(".tmp."),
                      std::string::npos);
    }
}

TEST(WorldSaveService, OverlappingRegionMergesRetainBothWritersRecords) {
    const TempSaveDir save_dir("overlapping_merges");
    const auto region = save_dir.path() / "records.lmr";
    const std::array<WorldSaveService::ContainerRecord, 2> records{{
        {.id = 7, .lod_level = 1, .flags = 0, .payload = "first"},
        {.id = 8, .lod_level = 1, .flags = 0, .payload = "second"},
    }};
    Luminumbra::world::WorldClock clock;
    Luminumbra::world::ActiveRegionLedger ledger;
    ledger.set_local_anchor(Vec3(1, 2, 3));
    OverlapObservation observation;
    ActiveOverlapObservation() = &observation;
    WorldSaveService::set_before_active_regions_replace_for_testing(&HoldStagedLedger);
    bool ledger_saved = false;
    std::array<bool, 2> merged{false, false};
    std::atomic<unsigned> completed{0};
    std::jthread ledger_writer([&] {
        ledger_saved = WorldSaveService::save_active_regions(ledger, clock, save_dir.path());
    });
    observation.staged.arrive_and_wait();
    const auto merge = [&](std::size_t index) {
        merged[index] = WorldSaveService::upsert_container_records(region, {records[index]});
        completed.fetch_add(1, std::memory_order_release);
    };
    // Two region writers overlap at the same destination while a third writer
    // holds ownership. Releasing the barrier permits either merge order.
    std::jthread first_writer(merge, 0u);
    std::jthread second_writer(merge, 1u);
    while (WorldSaveService::waiting_save_writers() < 2u &&
           completed.load(std::memory_order_acquire) == 0u)
        std::this_thread::yield();
    EXPECT_EQ(completed.load(std::memory_order_acquire), 0u);
    EXPECT_EQ(WorldSaveService::waiting_save_writers(), 2u);
    observation.release.arrive_and_wait();
    ledger_writer.join();
    first_writer.join();
    second_writer.join();
    WorldSaveService::set_before_active_regions_replace_for_testing(nullptr);
    ActiveOverlapObservation() = nullptr;
    EXPECT_TRUE(ledger_saved);
    EXPECT_TRUE(merged[0]);
    EXPECT_TRUE(merged[1]);
    EXPECT_EQ(WorldSaveService::waiting_save_writers(), 0u);
    std::vector<WorldSaveService::ContainerRecord> loaded;
    ASSERT_TRUE(WorldSaveService::read_container_records(region, loaded));
    ASSERT_EQ(loaded.size(), records.size());
    for (std::size_t index = 0; index < records.size(); ++index) {
        EXPECT_EQ(loaded[index].id, records[index].id);
        EXPECT_EQ(loaded[index].payload, records[index].payload);
    }
}

TEST(WorldSaveService, SharedWritesAndEmptyRemovalReclaimStagingOnEverySave) {
    const TempSaveDir save_dir("shared_cleanup");
    const auto file = save_dir.path() / Luminumbra::Filesystem::PathFromUtf8("aether_\xc3\xa9.efs");
    auto abandoned = file;
    abandoned += ".tmp.abandoned";
    const auto unrelated = save_dir.path() / "other.efs.tmp.abandoned";
    auto retained_directory = file;
    retained_directory += ".tmp.directory";
    std::filesystem::create_directory(retained_directory);
    ASSERT_TRUE(WorldSaveService::write_file_durably(unrelated, "keep"));
    for (const bool empty : {false, true}) {
        ASSERT_TRUE(WorldSaveService::write_file_durably(abandoned, "partial field"));
        if (empty) {
            ASSERT_TRUE(WorldSaveService::remove_file_and_staging(file));
        } else {
            ASSERT_TRUE(WorldSaveService::write_file_durably(file, "complete field"));
        }
        EXPECT_FALSE(std::filesystem::exists(abandoned));
        EXPECT_TRUE(std::filesystem::exists(unrelated));
        EXPECT_TRUE(std::filesystem::is_directory(retained_directory));
        EXPECT_EQ(std::filesystem::exists(file), !empty);
    }
    const auto missing = save_dir.path() / "missing/field.efs";
    ASSERT_TRUE(WorldSaveService::remove_file_and_staging(missing));
    EXPECT_FALSE(std::filesystem::exists(missing.parent_path()));
    std::filesystem::create_directory(file);
    ASSERT_TRUE(WorldSaveService::write_file_durably(file / "keep", "occupied"));
    EXPECT_FALSE(WorldSaveService::remove_file_and_staging(file));
    EXPECT_TRUE(std::filesystem::exists(file / "keep"));
}

TEST(WorldSaveService, StagingCleanupPreservesLinkedAndNonRegularOccupants) {
    const TempSaveDir save_dir("staging_bound_cleanup");
    const auto destination = save_dir.path() / "aether_state.efs";
    const auto external = save_dir.path() / "retained";
    const auto linked = save_dir.path() / "aether_state.efs.tmp.linked";
    const auto directory = save_dir.path() / "aether_state.efs.tmp.directory";
    const auto partial = save_dir.path() / "aether_state.efs.tmp.partial";
    ASSERT_TRUE(WorldSaveService::write_file_durably(external, "foreign bytes"));
    std::filesystem::create_hard_link(external, linked);
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    ASSERT_TRUE(WorldSaveService::write_file_durably(partial, "partial bytes"));
    WorldSaveService::reclaim_file_staging(destination);
    EXPECT_TRUE(std::filesystem::equivalent(external, linked));
    EXPECT_EQ(ReadFileBytes(external), "foreign bytes");
    EXPECT_TRUE(std::filesystem::is_directory(directory));
    EXPECT_FALSE(std::filesystem::exists(partial));
    EXPECT_FALSE(std::filesystem::exists(destination));
}

TEST(WorldSaveService, EmptyLedgerAndMetadataSavesReclaimBeforeTheirWriteOrNoop) {
    const TempSaveDir save_dir("empty_ledger_cleanup");
    const auto ledger_staging = save_dir.path() / "active-regions.arl.tmp.abandoned";
    ASSERT_TRUE(WorldSaveService::write_file_durably(ledger_staging, "partial ledger"));
    Luminumbra::world::ActiveRegionLedger ledger;
    const Luminumbra::world::WorldClock clock;
    ASSERT_TRUE(WorldSaveService::save_active_regions(ledger, clock, save_dir.path()));
    EXPECT_EQ(ReadFileBytes(ledger_staging), "partial ledger");
    EXPECT_FALSE(std::filesystem::exists(WorldSaveService::region_directory(save_dir.path())));
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(save_dir.path()),
                            std::filesystem::directory_iterator{}),
              1);

    const auto metadata_staging = save_dir.path() / "world_info.json.tmp.abandoned";
    // Exercise cleanup with supported clock metadata; opaque legacy bytes have
    // a separate compatibility contract and do not test the clock writer.
    nlohmann::json metadata = {{"container_version", WorldSaveService::kContainerVersion},
                               {"extension", {{"unknown", "preserved"}}}};
    clock.write_metadata(metadata);
    ASSERT_TRUE(WorldSaveService::write_file_durably(metadata_staging, "partial metadata"));
    ASSERT_TRUE(WorldSaveService::save_metadata(metadata.dump(), save_dir.path()));
    const auto valid = ReadFileBytes(save_dir.path() / "world_info.json");
    ASSERT_EQ(nlohmann::json::parse(valid), metadata);
    EXPECT_FALSE(std::filesystem::exists(metadata_staging));
    ASSERT_TRUE(WorldSaveService::write_file_durably(metadata_staging, "partial metadata"));
    for (const auto* invalid : {R"({"simulationTick":"invalid"})",
                                R"({"calendar":{"dayLengthTicks":12000}})",
                                R"({"simulationTick":18446744073709551615})"}) {
        SCOPED_TRACE(invalid);
        EXPECT_FALSE(WorldSaveService::save_metadata(invalid, save_dir.path()));
        EXPECT_EQ(ReadFileBytes(save_dir.path() / "world_info.json"), valid);
        EXPECT_EQ(ReadFileBytes(metadata_staging), "partial metadata");
    }
    // Refusal happens before cleanup or publication; prior bytes remain complete.
    EXPECT_TRUE(std::filesystem::exists(metadata_staging));
    std::ifstream input(save_dir.path() / "world_info.json", std::ios::binary);
    std::ostringstream bytes;
    bytes << input.rdbuf();
    EXPECT_EQ(bytes.str(), valid);
    input.close();
    EXPECT_FALSE(WorldSaveService::save_metadata("{}", save_dir.path()));
    EXPECT_TRUE(std::filesystem::exists(metadata_staging));
    EXPECT_EQ(ReadFileBytes(save_dir.path() / "world_info.json"), valid);
    ASSERT_TRUE(WorldSaveService::save_metadata(valid, save_dir.path()));
    EXPECT_FALSE(std::filesystem::exists(metadata_staging));
    EXPECT_EQ(ReadFileBytes(save_dir.path() / "world_info.json"), valid);
    ASSERT_TRUE(WorldSaveService::write_file_durably(ledger_staging, "partial ledger"));
    ASSERT_TRUE(WorldSaveService::save_active_regions(ledger, clock, save_dir.path()));
    EXPECT_EQ(ReadFileBytes(ledger_staging), "partial ledger");
    EXPECT_FALSE(std::filesystem::exists(WorldSaveService::region_directory(save_dir.path())));
}

namespace {
void WriteStagingFixture(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    ASSERT_TRUE(output);
}

struct CapturedLedgerStaging {
    std::filesystem::path directory;
    std::filesystem::path payload;
    std::filesystem::path record;
    std::string ownership;
    static CapturedLedgerStaging& current() {
        static CapturedLedgerStaging capture;
        return capture;
    }
};

void LeaveOwnedLedgerStaging(const std::filesystem::path& directory) {
    auto& capture = CapturedLedgerStaging::current();
    capture = {.directory = directory, .payload = {}, .record = {}, .ownership = {}};
    Luminumbra::world::ActiveRegionLedger ledger;
    ledger.set_local_anchor(Vec3(1, 2, 3));
    WorldSaveService::set_before_active_regions_replace_for_testing([] {
        auto& staged = CapturedLedgerStaging::current();
        for (const auto& entry : std::filesystem::directory_iterator(staged.directory)) {
            if (entry.path().filename().string().starts_with("active-regions.arl.tmp.") &&
                entry.path().extension() != ".owner") {
                staged.payload = entry.path();
                staged.record = entry.path();
                staged.record += ".owner";
                staged.ownership = ReadFileBytes(staged.record);
                // Invalidate the record so rollback preserves the pair. Restore
                // the same record instance after all writer handles are closed.
                WriteStagingFixture(staged.record, "interrupted record");
                break;
            }
        }
        return false;
    });
    const bool saved =
        WorldSaveService::save_active_regions(ledger, Luminumbra::world::WorldClock{}, directory);
    WorldSaveService::set_before_active_regions_replace_for_testing(nullptr);
    ASSERT_FALSE(saved);
    ASSERT_FALSE(capture.payload.empty());
    ASSERT_FALSE(capture.ownership.empty());
    ASSERT_TRUE(std::filesystem::exists(capture.payload));
    ASSERT_NO_FATAL_FAILURE(WriteStagingFixture(capture.record, capture.ownership));
}

void SaveLedgerAfterStaging(const std::filesystem::path& directory, bool empty = false) {
    Luminumbra::world::ActiveRegionLedger ledger;
    if (!empty)
        ledger.set_local_anchor(Vec3(4, 5, 6));
    ASSERT_TRUE(
        WorldSaveService::save_active_regions(ledger, Luminumbra::world::WorldClock{}, directory));
    ASSERT_TRUE(WorldSaveService::validate_save(directory));
}

void LeavePublishedLedgerSidecar(const std::filesystem::path& directory) {
    ASSERT_NO_FATAL_FAILURE(LeaveOwnedLedgerStaging(directory));
    const auto& capture = CapturedLedgerStaging::current();
    const auto destination = WorldSaveService::active_regions_path(directory);
    // Reproduce termination between publication and retiring the same sidecar.
    // All writing handles are closed, including on Windows.
    std::filesystem::create_directories(destination.parent_path());
    std::filesystem::remove(destination);
    std::filesystem::rename(capture.payload, destination);
    ASSERT_FALSE(std::filesystem::exists(capture.payload));
    ASSERT_EQ(ReadFileBytes(capture.record), capture.ownership);
    ASSERT_TRUE(WorldSaveService::validate_save(directory));
}

void RefreshOwnershipChecksum(std::string& bytes) {
    ASSERT_GE(bytes.size(), 8u);
    std::uint64_t checksum = 14695981039346656037ull;
    for (std::size_t index = 0; index < bytes.size() - 8; ++index) {
        checksum ^= static_cast<unsigned char>(bytes[index]);
        checksum *= 1099511628211ull;
    }
    for (int index = 0; index < 8; ++index)
        bytes[bytes.size() - 8 + static_cast<std::size_t>(index)] =
            static_cast<char>((checksum >> (index * 8)) & 255u);
}

void SetOwnershipWord(std::string& bytes, std::size_t offset, std::uint64_t value) {
    ASSERT_LE(offset, bytes.size());
    ASSERT_GE(bytes.size() - offset, 8u);
    for (unsigned index = 0; index < 8; ++index)
        bytes[offset + index] = static_cast<char>((value >> (index * 8)) & 255u);
}

void BindOwnershipInstance(std::string& bytes, const std::filesystem::path& path) {
    std::array<std::uint64_t, 3> identity{};
#if defined(_WIN32)
    const HANDLE handle = ::CreateFileW(path.c_str(),
                                        FILE_READ_ATTRIBUTES,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_FLAG_OPEN_REPARSE_POINT,
                                        nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    BY_HANDLE_FILE_INFORMATION information{};
    const bool identified = ::GetFileInformationByHandle(handle, &information) != 0;
    const bool closed = ::CloseHandle(handle) != 0;
    ASSERT_TRUE(identified);
    ASSERT_TRUE(closed);
    identity = {
        information.dwVolumeSerialNumber, information.nFileIndexHigh, information.nFileIndexLow};
#else
    struct stat information {};
    ASSERT_EQ(::lstat(path.c_str(), &information), 0);
    ASSERT_TRUE(S_ISREG(information.st_mode));
    identity = {static_cast<std::uint64_t>(information.st_dev),
                static_cast<std::uint64_t>(information.st_ino),
                0};
#endif
    // AROWN001: magic + three directory words + six payload words.
    for (std::size_t index = 0; index < identity.size(); ++index)
        ASSERT_NO_FATAL_FAILURE(SetOwnershipWord(bytes, 80 + index * 8, identity[index]));
}

struct InterruptedLedgerStaging {
    std::filesystem::path payload;
    std::filesystem::path owner;
    std::filesystem::path claim;
    std::string claim_bytes;
    std::string payload_bytes;
};

void LeaveInterruptedLedgerStaging(const std::filesystem::path& directory,
                                   int point,
                                   InterruptedLedgerStaging& interrupted) {
    ASSERT_NO_FATAL_FAILURE(LeaveOwnedLedgerStaging(directory));
    const auto captured = CapturedLedgerStaging::current();
    interrupted = {.payload = captured.payload,
                   .owner = captured.record,
                   .claim = captured.payload,
                   .claim_bytes = captured.ownership,
                   .payload_bytes = ReadFileBytes(captured.payload)};
    interrupted.claim += ".claim";
    ASSERT_NO_FATAL_FAILURE(WriteStagingFixture(interrupted.claim, {}));
    ASSERT_NO_FATAL_FAILURE(BindOwnershipInstance(interrupted.claim_bytes, interrupted.claim));
    // Incomplete claims retain only the reserved payload's three stable words.
    for (std::size_t index = 3; index < 6; ++index)
        ASSERT_NO_FATAL_FAILURE(SetOwnershipWord(interrupted.claim_bytes, 32 + index * 8, 0));
    ASSERT_NO_FATAL_FAILURE(RefreshOwnershipChecksum(interrupted.claim_bytes));
    ASSERT_NO_FATAL_FAILURE(WriteStagingFixture(interrupted.claim, interrupted.claim_bytes));
    if (point == 0) {
        // After the durable claim, before promotion: the reserved inode still
        // contains a self-identifying sidecar and has no payload name.
        ASSERT_TRUE(std::filesystem::remove(interrupted.owner));
        std::filesystem::rename(interrupted.payload, interrupted.owner);
        auto seed = captured.ownership;
        for (std::size_t index = 0; index < 6; ++index)
            ASSERT_NO_FATAL_FAILURE(SetOwnershipWord(seed, 32 + index * 8, 0));
        ASSERT_NO_FATAL_FAILURE(BindOwnershipInstance(seed, interrupted.owner));
        ASSERT_NO_FATAL_FAILURE(RefreshOwnershipChecksum(seed));
        ASSERT_NO_FATAL_FAILURE(WriteStagingFixture(interrupted.owner, seed));
    } else if (point == 1) {
        // After promotion, before the payload write or completion record.
        ASSERT_TRUE(std::filesystem::remove(interrupted.owner));
        ASSERT_NO_FATAL_FAILURE(WriteStagingFixture(interrupted.payload, {}));
    }
    // point == 2 leaves the completed record and payload unchanged, just before
    // the incomplete claim is retired. No production testing seam is needed.
}
} // namespace

TEST(WorldSaveService, LedgerStagingRepeatedClaimInterruptionsDoNotAccumulatePayloads) {
    // Three durable boundaries, eight retries each, and both empty/nonempty
    // recovery saves. Each directory contains at most one interrupted pair.
    for (const int point : {0, 1, 2}) {
        const TempSaveDir save_dir("ledger_claim_interruptions");
        for (int attempt = 0; attempt < 8; ++attempt) {
            SCOPED_TRACE(point);
            SCOPED_TRACE(attempt);
            InterruptedLedgerStaging interrupted;
            ASSERT_NO_FATAL_FAILURE(
                LeaveInterruptedLedgerStaging(save_dir.path(), point, interrupted));
            ASSERT_NO_FATAL_FAILURE(SaveLedgerAfterStaging(save_dir.path(), attempt % 2 == 0));
            EXPECT_FALSE(std::filesystem::exists(interrupted.payload));
            EXPECT_FALSE(std::filesystem::exists(interrupted.owner));
            EXPECT_FALSE(std::filesystem::exists(interrupted.claim));
            for (const auto& entry : std::filesystem::directory_iterator(save_dir.path()))
                EXPECT_FALSE(
                    entry.path().filename().string().starts_with("active-regions.arl.tmp."));
        }
    }
}

TEST(WorldSaveService, LedgerStagingIncompleteClaimsRequireCanonicalProofAndReservedIdentity) {
    // Valid control, empty/partial/oversized/unreadable-equivalent proof,
    // checksum, directory/instance/name/nonce bindings, unknown state and a
    // substituted payload. The valid control fails without claim recovery.
    for (int mutation = 0; mutation < 14; ++mutation) {
        SCOPED_TRACE(mutation);
        const TempSaveDir save_dir("ledger_claim_proof");
        InterruptedLedgerStaging interrupted;
        ASSERT_NO_FATAL_FAILURE(LeaveInterruptedLedgerStaging(save_dir.path(), 1, interrupted));
        auto changed = interrupted.claim_bytes;
        if (mutation == 1)
            changed.clear();
        else if (mutation == 2)
            changed.resize(79);
        else if (mutation == 3)
            changed.assign(513, 'x');
        else if (mutation >= 4 && mutation <= 10) {
            const std::array<std::size_t, 7> offsets{0, 8, 80, 112, changed.size() - 9, 56, 32};
            changed[offsets[static_cast<std::size_t>(mutation - 4)]] ^= 1;
            ASSERT_NO_FATAL_FAILURE(RefreshOwnershipChecksum(changed));
        } else if (mutation == 11) {
            changed.back() ^= 1;
        } else if (mutation == 12) {
            // Keep the claimed inode alive so the replacement cannot reuse it.
            std::filesystem::rename(interrupted.payload, save_dir.path() / "held-payload");
            ASSERT_NO_FATAL_FAILURE(WriteStagingFixture(interrupted.payload, "foreign payload"));
        } else if (mutation == 13) {
            // A checksum-valid record without a reserved file identity cannot
            // authorize deletion, even at the correct unique filename.
            for (std::size_t index = 0; index < 3; ++index)
                ASSERT_NO_FATAL_FAILURE(SetOwnershipWord(changed, 32 + index * 8, 0));
            ASSERT_NO_FATAL_FAILURE(RefreshOwnershipChecksum(changed));
        }
        ASSERT_NO_FATAL_FAILURE(WriteStagingFixture(interrupted.claim, changed));
        ASSERT_NO_FATAL_FAILURE(SaveLedgerAfterStaging(save_dir.path()));
        if (mutation == 0) {
            EXPECT_FALSE(std::filesystem::exists(interrupted.payload));
            EXPECT_FALSE(std::filesystem::exists(interrupted.claim));
        } else {
            EXPECT_TRUE(std::filesystem::is_regular_file(interrupted.payload));
            EXPECT_EQ(ReadFileBytes(interrupted.claim), changed);
            EXPECT_EQ(ReadFileBytes(interrupted.payload), mutation == 12 ? "foreign payload" : "");
        }
    }
}

TEST(WorldSaveService, LedgerStagingIncompleteClaimSurvivesTornCompletionAndPayloadWrites) {
    for (const bool complete_payload : {false, true}) {
        const TempSaveDir save_dir("ledger_torn_completion");
        InterruptedLedgerStaging interrupted;
        ASSERT_NO_FATAL_FAILURE(LeaveInterruptedLedgerStaging(save_dir.path(), 1, interrupted));
        ASSERT_NO_FATAL_FAILURE(WriteStagingFixture(
            interrupted.payload, complete_payload ? interrupted.payload_bytes : "partial ledger"));
        ASSERT_NO_FATAL_FAILURE(WriteStagingFixture(interrupted.owner, "partial ownership"));
        ASSERT_NO_FATAL_FAILURE(SaveLedgerAfterStaging(save_dir.path()));
        EXPECT_FALSE(std::filesystem::exists(interrupted.payload));
        EXPECT_FALSE(std::filesystem::exists(interrupted.claim));
        // The partial sidecar cannot prove its own identity and must survive;
        // it contains no ledger copy.
        EXPECT_EQ(ReadFileBytes(interrupted.owner), "partial ownership");
    }
}

#if !defined(_WIN32)
TEST(WorldSaveService, LedgerStagingIncompleteClaimsRecoverOnlyTheirReservationLinks) {
    for (const bool own_reservation : {false, true}) {
        const TempSaveDir save_dir("ledger_claim_links");
        InterruptedLedgerStaging interrupted;
        ASSERT_NO_FATAL_FAILURE(LeaveInterruptedLedgerStaging(save_dir.path(), 1, interrupted));
        const auto other = own_reservation ? interrupted.owner : save_dir.path() / "foreign-link";
        std::filesystem::create_hard_link(interrupted.payload, other);
        ASSERT_NO_FATAL_FAILURE(SaveLedgerAfterStaging(save_dir.path()));
        if (!own_reservation) {
            EXPECT_TRUE(std::filesystem::exists(interrupted.payload));
            EXPECT_EQ(ReadFileBytes(interrupted.claim), interrupted.claim_bytes);
            ASSERT_TRUE(std::filesystem::remove(other));
            ASSERT_NO_FATAL_FAILURE(SaveLedgerAfterStaging(save_dir.path()));
        }
        EXPECT_FALSE(std::filesystem::exists(interrupted.payload));
        EXPECT_FALSE(std::filesystem::exists(interrupted.claim));
        EXPECT_FALSE(std::filesystem::exists(other));
    }
}

TEST(WorldSaveService, LedgerStagingIncompleteClaimsPreserveNonregularLeavesUntilRestored) {
    // Each leaf (claim/payload) is replaced by a symlink, FIFO or directory.
    // Restoration keeps the original inode alive and proves recovery on retry.
    for (int replacement = 0; replacement < 6; ++replacement) {
        SCOPED_TRACE(replacement);
        const TempSaveDir save_dir("ledger_claim_leaf");
        InterruptedLedgerStaging interrupted;
        ASSERT_NO_FATAL_FAILURE(LeaveInterruptedLedgerStaging(save_dir.path(), 1, interrupted));
        const auto leaf = replacement % 2 == 0 ? interrupted.claim : interrupted.payload;
        const auto held = save_dir.path() / "held-leaf";
        const auto target = save_dir.path() / "foreign-target";
        ASSERT_NO_FATAL_FAILURE(WriteStagingFixture(target, "foreign bytes"));
        std::filesystem::rename(leaf, held);
        if (replacement < 2)
            std::filesystem::create_symlink(target.filename(), leaf);
        else if (replacement < 4)
            ASSERT_EQ(::mkfifo(leaf.c_str(), 0600), 0);
        else
            ASSERT_TRUE(std::filesystem::create_directory(leaf));
        const auto kind = std::filesystem::symlink_status(leaf).type();
        ASSERT_NO_FATAL_FAILURE(SaveLedgerAfterStaging(save_dir.path()));
        EXPECT_EQ(std::filesystem::symlink_status(leaf).type(), kind);
        EXPECT_EQ(ReadFileBytes(target), "foreign bytes");
        ASSERT_TRUE(std::filesystem::remove(leaf));
        std::filesystem::rename(held, leaf);
        ASSERT_NO_FATAL_FAILURE(SaveLedgerAfterStaging(save_dir.path()));
        EXPECT_FALSE(std::filesystem::exists(interrupted.payload));
        EXPECT_FALSE(std::filesystem::exists(interrupted.claim));
    }
}
#else
TEST(WorldSaveService, LedgerStagingIncompleteClaimsPreserveUnreadableProofUntilRetry) {
    const TempSaveDir save_dir("locked_ledger_claim");
    InterruptedLedgerStaging interrupted;
    ASSERT_NO_FATAL_FAILURE(LeaveInterruptedLedgerStaging(save_dir.path(), 1, interrupted));
    const HANDLE locked = ::CreateFileW(interrupted.claim.c_str(),
                                        GENERIC_READ,
                                        0,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_FLAG_OPEN_REPARSE_POINT,
                                        nullptr);
    ASSERT_NE(locked, INVALID_HANDLE_VALUE);
    SaveLedgerAfterStaging(save_dir.path());
    EXPECT_NE(::CloseHandle(locked), 0);
    EXPECT_TRUE(std::filesystem::exists(interrupted.payload));
    EXPECT_EQ(ReadFileBytes(interrupted.claim), interrupted.claim_bytes);
    ASSERT_NO_FATAL_FAILURE(SaveLedgerAfterStaging(save_dir.path()));
    EXPECT_FALSE(std::filesystem::exists(interrupted.payload));
    EXPECT_FALSE(std::filesystem::exists(interrupted.claim));
}
#endif

TEST(WorldSaveService, LedgerStagingPublishesCreatesAndReplacementsWithoutDebris) {
    const TempSaveDir save_dir("ledger_repeated_publication");
    Luminumbra::world::ActiveRegionLedger ledger;
    const auto destination = WorldSaveService::active_regions_path(save_dir.path());
    ASSERT_FALSE(std::filesystem::exists(destination));
    // Exercise missing-destination publication, replacement, and an empty ledger.
    for (int pass = 0; pass < 3; ++pass) {
        SCOPED_TRACE(pass);
        ledger = Luminumbra::world::ActiveRegionLedger{};
        if (pass < 2)
            ledger.set_local_anchor(Vec3(static_cast<float>(pass), 2, 3));
        std::vector<std::string> errors;
        ASSERT_TRUE(WorldSaveService::save_active_regions(
            ledger, Luminumbra::world::WorldClock{}, save_dir.path(), &errors));
        EXPECT_TRUE(errors.empty());
        EXPECT_EQ(ReadFileBytes(destination), ledger.encode());
        Luminumbra::world::ActiveRegionLedger loaded;
        ASSERT_TRUE(WorldSaveService::load_active_regions(loaded, save_dir.path(), &errors));
        EXPECT_EQ(loaded.encode(), ledger.encode());
        for (const auto& entry : std::filesystem::directory_iterator(save_dir.path()))
            EXPECT_FALSE(entry.path().filename().string().starts_with("active-regions.arl.tmp."));
    }
}

namespace {
void CheckStagingRegistrationRefusals(
    Luminumbra::Persistence::CreatedWorldArtifacts& artifacts,
    const std::filesystem::path& directory,
    const std::filesystem::path& payload,
    const std::filesystem::path& foreign,
    Luminumbra::Persistence::CreatedWorldArtifacts::NativeFile native,
    Luminumbra::Persistence::CreatedWorldArtifacts::NativeFile invalid_native) {
    EXPECT_THROW(artifacts.record_staged_file(directory / "missing", native), std::runtime_error);
    EXPECT_THROW(artifacts.record_staged_file(foreign, native), std::runtime_error);
    EXPECT_THROW(artifacts.record_staged_file(directory, native), std::runtime_error);
    EXPECT_THROW(artifacts.record_staged_file(payload, invalid_native), std::runtime_error);
}

void CheckStagingRegistration(bool scoped_removal) {
    SCOPED_TRACE(scoped_removal);
    const TempSaveDir save_dir("creation_staging_registration");
    const auto directory = save_dir.path() / "created-world";
    Luminumbra::Persistence::CreatedWorldArtifacts artifacts;
    artifacts.create_root(directory);
    const auto payload = directory / "payload";
#if defined(_WIN32)
    const auto close_file = [](HANDLE handle) {
        ::CloseHandle(handle);
    };
    std::unique_ptr<void, decltype(close_file)> file(
        ::CreateFileW(payload.c_str(),
                      GENERIC_READ | GENERIC_WRITE | DELETE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      nullptr,
                      CREATE_NEW,
                      FILE_ATTRIBUTE_NORMAL,
                      nullptr),
        close_file);
    ASSERT_NE(file.get(), INVALID_HANDLE_VALUE);
    const auto native = file.get();
    const auto invalid_native = INVALID_HANDLE_VALUE;
#else
    const auto close_file = [](std::FILE* stream) {
        std::fclose(stream);
    };
    std::unique_ptr<std::FILE, decltype(close_file)> file(std::fopen(payload.c_str(), "wb+"),
                                                          close_file);
    ASSERT_TRUE(file);
    const auto native = ::fileno(file.get());
    const int invalid_native = -1;
#endif
    const auto foreign = directory / "foreign";
    ASSERT_NO_FATAL_FAILURE(WriteStagingFixture(foreign, "foreign bytes"));
    ASSERT_NO_FATAL_FAILURE(CheckStagingRegistrationRefusals(
        artifacts, directory, payload, foreign, native, invalid_native));
    EXPECT_EQ(ReadFileBytes(foreign), "foreign bytes");
    artifacts.record_staged_file(payload, native);
#if defined(_WIN32)
    std::unique_ptr<void, decltype(close_file)> foreign_file(
        ::CreateFileW(foreign.c_str(),
                      FILE_READ_ATTRIBUTES,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      nullptr,
                      OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL,
                      nullptr),
        close_file);
    ASSERT_NE(foreign_file.get(), INVALID_HANDLE_VALUE);
    const auto foreign_native = foreign_file.get();
#else
    std::unique_ptr<std::FILE, decltype(close_file)> foreign_file(std::fopen(foreign.c_str(), "rb"),
                                                                  close_file);
    ASSERT_TRUE(foreign_file);
    const auto foreign_native = ::fileno(foreign_file.get());
#endif
    EXPECT_THROW(artifacts.forget_removed_staged_file(payload, foreign_native), std::runtime_error);
    foreign_file.reset();
    EXPECT_EQ(ReadFileBytes(foreign), "foreign bytes");
    ASSERT_TRUE(std::filesystem::remove(foreign));
    EXPECT_THROW(artifacts.forget_removed_staged_file(directory / "missing", native),
                 std::runtime_error);
    EXPECT_THROW(artifacts.forget_removed_staged_file(directory, native), std::runtime_error);
    EXPECT_THROW(artifacts.forget_removed_staged_file(payload, invalid_native), std::runtime_error);
    if (scoped_removal) {
#if defined(_WIN32)
        FILE_DISPOSITION_INFO disposition{.DeleteFile = TRUE};
        ASSERT_TRUE(::SetFileInformationByHandle(
            native, FileDispositionInfo, &disposition, static_cast<DWORD>(sizeof(disposition))));
#else
        ASSERT_EQ(::unlink(payload.c_str()), 0);
#endif
        artifacts.forget_removed_staged_file(payload, native);
        EXPECT_THROW(artifacts.forget_removed_staged_file(payload, native), std::runtime_error);
    }
    file.reset();
    if (scoped_removal) {
        EXPECT_TRUE(std::filesystem::is_empty(directory));
    }
    EXPECT_TRUE(artifacts.rollback().empty());
    EXPECT_FALSE(std::filesystem::exists(directory));
}
} // namespace

TEST(WorldSaveService, StagedFileRegistrationRetainsIdentityUntilRollback) {
    CheckStagingRegistration(false);
}

TEST(WorldSaveService, RemovedStagedFileReleasesJournalIdentityBeforeRollback) {
    CheckStagingRegistration(true);
}

TEST(WorldSaveService, CreationLedgerStagingCleansFailuresAndJournalsPublication) {
    struct Observation {
        std::filesystem::path directory;
        std::filesystem::path payload;
        Luminumbra::Persistence::CreatedWorldArtifacts* artifacts = nullptr;
        bool allow_publication = false;
        bool publish_before_throw = false;
        unsigned hook_calls = 0;
        static Observation& current() {
            static Observation observation;
            return observation;
        }
    };
    struct HookReset {
        ~HookReset() {
            WorldSaveService::set_before_active_regions_replace_for_testing(nullptr);
            Luminumbra::Persistence::detail::set_file_publication_hook_for_testing(nullptr);
        }
    };
    enum class Outcome {
        Interrupted,
        JournalFull,
        PublicationThrows,
        PublicationThrowsAfterInstall,
        Published,
#ifndef _WIN32
        ClaimFlushFailed,
        PublicationFlushFailed,
#endif
    };
    for (const auto outcome : {
             Outcome::Interrupted,
             Outcome::JournalFull,
             Outcome::PublicationThrows,
             Outcome::PublicationThrowsAfterInstall,
             Outcome::Published,
#ifndef _WIN32
             Outcome::ClaimFlushFailed,
             Outcome::PublicationFlushFailed,
#endif
         }) {
        SCOPED_TRACE(static_cast<int>(outcome));
        const TempSaveDir save_dir("creation_ledger_staging");
        const auto directory = save_dir.path() / "created-world";
        Luminumbra::Persistence::CreatedWorldArtifacts artifacts;
        artifacts.create_root(directory);
        // Legacy helpers use nine slots; indexed creation reserves two more for
        // its index. Three extra owned files exhaust that full eleven-slot bound.
        ASSERT_TRUE(WorldSaveService::write_file_durably(
            directory / "preset.json", "preset bytes\n", nullptr, &artifacts));
        if (outcome == Outcome::JournalFull) {
            artifacts.write_file(directory / "partial.bin", "partial bytes\n");
            artifacts.write_file(directory / "partial-second.bin", "partial bytes\n");
            artifacts.write_file(directory / "partial-third.bin", "partial bytes\n");
        }
        const Luminumbra::world::WorldClock clock;
        Luminumbra::world::ActiveRegionLedger ledger;
        ledger.set_local_anchor(Vec3(1, 2, 3));
        nlohmann::json metadata = {{"container_version", WorldSaveService::kContainerVersion}};
        clock.write_metadata(metadata);
        ASSERT_TRUE(
            WorldSaveService::save_metadata(metadata.dump(), directory, nullptr, &artifacts));
        auto& observation = Observation::current();
        observation = {.directory = directory,
                       .payload = {},
                       .artifacts = &artifacts,
                       .allow_publication = outcome != Outcome::Interrupted,
                       .publish_before_throw = outcome == Outcome::PublicationThrowsAfterInstall,
                       .hook_calls = 0};
        const HookReset hook_reset;
        const DirectoryFlushReset flush_reset;
        WorldSaveService::set_before_active_regions_replace_for_testing([] {
            auto& observed = Observation::current();
            ++observed.hook_calls;
            unsigned payloads = 0;
            unsigned owners = 0;
            for (const auto& entry : std::filesystem::directory_iterator(observed.directory)) {
                if (!entry.path().filename().string().starts_with("active-regions.arl.tmp."))
                    continue;
                EXPECT_NE(entry.path().extension(), ".claim");
                if (entry.path().extension() == ".owner")
                    ++owners;
                else {
                    ++payloads;
                    observed.payload = entry.path();
                }
            }
            EXPECT_EQ(payloads, 1u);
            EXPECT_EQ(owners, 1u);
            return observed.allow_publication;
        });
        if (outcome == Outcome::PublicationThrows ||
            outcome == Outcome::PublicationThrowsAfterInstall) {
            Luminumbra::Persistence::detail::set_file_publication_hook_for_testing(
                [](const std::filesystem::path& destination, bool) -> bool {
                    auto& observed = Observation::current();
                    if (observed.publish_before_throw)
                        observed.artifacts->publish_file(observed.payload, destination);
                    throw std::runtime_error("publication refused");
                });
        }
#ifndef _WIN32
        int staging_ancestors = 0;
        for (auto ancestor = directory; !ancestor.empty() && ancestor != ancestor.root_path();
             ancestor = ancestor.parent_path())
            ++staging_ancestors;
        if (outcome == Outcome::ClaimFlushFailed) {
            // The durable claim precedes payload promotion and ledger bytes.
            WorldSaveService::set_directory_flush_failure_for_testing(staging_ancestors);
        } else if (outcome == Outcome::PublicationFlushFailed) {
            // Four staging flushes, then the destination's ancestor entries.
            WorldSaveService::set_directory_flush_failure_for_testing(2 * staging_ancestors + 6);
        }
#endif
        std::vector<std::string> errors;
        const bool saved =
            WorldSaveService::save_active_regions(ledger, clock, directory, &errors, &artifacts);
        EXPECT_EQ(saved, outcome == Outcome::Published);
        EXPECT_EQ(errors.empty(), saved);
        unsigned expected_hook_calls = 1;
#ifndef _WIN32
        if (outcome == Outcome::ClaimFlushFailed)
            expected_hook_calls = 0;
#endif
        EXPECT_EQ(observation.hook_calls, expected_hook_calls);
        for (const auto& entry : std::filesystem::directory_iterator(directory))
            EXPECT_FALSE(entry.path().filename().string().starts_with("active-regions.arl.tmp."));
        const auto destination = WorldSaveService::active_regions_path(directory);
        bool installed = saved || outcome == Outcome::PublicationThrowsAfterInstall;
#ifndef _WIN32
        installed = installed || outcome == Outcome::PublicationFlushFailed;
#endif
        EXPECT_EQ(std::filesystem::exists(destination), installed);
        if (installed) {
            EXPECT_EQ(ReadFileBytes(destination), ledger.encode());
        }
        // A live ledger omitted from the journal makes rollback refuse, so
        // successful rollback also proves publication recorded its identity.
        EXPECT_TRUE(artifacts.rollback().empty());
        EXPECT_FALSE(std::filesystem::exists(directory));
    }
}

TEST(WorldSaveService, LedgerStagingReportsPartialAndEmptyOwnershipBeforeRetry) {
    for (const std::string& damaged : {std::string{}, std::string{"partial"}}) {
        const TempSaveDir save_dir("ledger_ownership_diagnostic");
        auto& capture = CapturedLedgerStaging::current();
        capture = {.directory = save_dir.path(), .payload = {}, .record = {}, .ownership = damaged};
        WorldSaveService::set_before_active_regions_replace_for_testing([] {
            auto& staged = CapturedLedgerStaging::current();
            for (const auto& entry : std::filesystem::directory_iterator(staged.directory)) {
                if (entry.path().filename().string().starts_with("active-regions.arl.tmp.") &&
                    entry.path().extension() != ".owner") {
                    staged.payload = entry.path();
                    staged.record = entry.path();
                    staged.record += ".owner";
                    WriteStagingFixture(staged.record, staged.ownership);
                    break;
                }
            }
            return true;
        });
        Luminumbra::world::ActiveRegionLedger ledger;
        ledger.set_local_anchor(Vec3(1, 2, 3));
        std::vector<std::string> errors;
        const bool saved = WorldSaveService::save_active_regions(
            ledger, Luminumbra::world::WorldClock{}, save_dir.path(), &errors);
        WorldSaveService::set_before_active_regions_replace_for_testing(nullptr);
        ASSERT_TRUE(saved);
        ASSERT_EQ(errors.size(), 1u);
        EXPECT_NE(errors.front().find("staging ownership bytes do not match"), std::string::npos);
        ASSERT_FALSE(capture.record.empty());
        EXPECT_EQ(ReadFileBytes(capture.record), damaged);
        EXPECT_TRUE(std::filesystem::exists(capture.payload));
        EXPECT_EQ(ReadFileBytes(WorldSaveService::active_regions_path(save_dir.path())),
                  ledger.encode());
    }
}

TEST(WorldSaveService, LedgerStagingPreservesUnknownOccupantsAndEmptySave) {
    const TempSaveDir save_dir("unknown_ledger_staging");
    const auto unknown = save_dir.path() / "active-regions.arl.tmp.abandoned";
    const auto directory = save_dir.path() / "active-regions.arl.tmp.directory";
    const auto orphan_record = save_dir.path() / "active-regions.arl.tmp.orphan.owner";
    const auto unicode =
        save_dir.path() / Luminumbra::Filesystem::PathFromUtf8("active-regions.arl.tmp.\xc3\xa9");
    auto unicode_record = unicode;
    unicode_record += ".owner";
    ASSERT_NO_FATAL_FAILURE(WriteStagingFixture(unicode, "foreign Unicode"));
    ASSERT_NO_FATAL_FAILURE(WriteStagingFixture(unicode_record, "foreign Unicode record"));
    ASSERT_NO_FATAL_FAILURE(WriteStagingFixture(unknown, "foreign bytes"));
    ASSERT_NO_FATAL_FAILURE(WriteStagingFixture(orphan_record, "foreign record"));
    std::filesystem::create_directory(directory);
    ASSERT_NO_FATAL_FAILURE(WriteStagingFixture(directory / "keep", "directory occupant"));
    for (const bool empty : {true, false}) {
        ASSERT_NO_FATAL_FAILURE(SaveLedgerAfterStaging(save_dir.path(), empty));
        EXPECT_EQ(ReadFileBytes(unicode), "foreign Unicode");
        EXPECT_EQ(ReadFileBytes(unicode_record), "foreign Unicode record");
        EXPECT_EQ(ReadFileBytes(unknown), "foreign bytes");
        EXPECT_EQ(ReadFileBytes(orphan_record), "foreign record");
        EXPECT_EQ(ReadFileBytes(directory / "keep"), "directory occupant");
    }
}

TEST(WorldSaveService, LedgerStagingReclaimsOnlyValidOwnedPairsIncludingEmptySave) {
    for (const bool empty : {false, true}) {
        const TempSaveDir save_dir("owned_ledger_staging");
        ASSERT_NO_FATAL_FAILURE(LeaveOwnedLedgerStaging(save_dir.path()));
        const auto capture = CapturedLedgerStaging::current();
        ASSERT_NO_FATAL_FAILURE(SaveLedgerAfterStaging(save_dir.path(), empty));
        EXPECT_FALSE(std::filesystem::exists(capture.payload));
        EXPECT_FALSE(std::filesystem::exists(capture.record));
        for (const auto& entry : std::filesystem::directory_iterator(save_dir.path()))
            EXPECT_FALSE(entry.path().filename().string().starts_with("active-regions.arl.tmp."));
    }
}

TEST(WorldSaveService, LedgerStagingReclaimsValidOrphanAfterPublication) {
    for (const bool empty : {false, true}) {
        const TempSaveDir save_dir("published_ledger_sidecar");
        ASSERT_NO_FATAL_FAILURE(LeavePublishedLedgerSidecar(save_dir.path()));
        const auto capture = CapturedLedgerStaging::current();
        const auto path = WorldSaveService::active_regions_path(save_dir.path());
        const auto published = ReadFileBytes(path);
        ASSERT_TRUE(
            WorldSaveService::remove_file_and_staging(save_dir.path() / "active-regions.arl"));
        EXPECT_FALSE(std::filesystem::exists(capture.record));
        EXPECT_EQ(ReadFileBytes(path), published);
        ASSERT_NO_FATAL_FAILURE(SaveLedgerAfterStaging(save_dir.path(), empty));
        EXPECT_FALSE(std::filesystem::exists(capture.payload));
        EXPECT_FALSE(std::filesystem::exists(capture.record));
    }
}

TEST(WorldSaveService, LedgerStagingPreservesInvalidOrphanOwnership) {
    for (const int mutation : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}) {
        SCOPED_TRACE(mutation);
        const TempSaveDir save_dir("invalid_orphan_ownership");
        ASSERT_NO_FATAL_FAILURE(LeavePublishedLedgerSidecar(save_dir.path()));
        const auto capture = CapturedLedgerStaging::current();
        auto changed = capture.ownership;
        ASSERT_GT(changed.size(), 152u);
        if (mutation == 0)
            changed.clear();
        else if (mutation == 1)
            changed.resize(79);
        else if (mutation == 2)
            changed.resize(changed.size() - 1);
        else if (mutation == 3)
            changed.assign(513, 'x');
        else {
            const std::array<std::size_t, 7> positions{
                0, 8, 80, 104, 112, changed.size() - 9, changed.size() - 1};
            changed[positions[static_cast<std::size_t>(mutation - 4)]] ^= 1;
            if (mutation != 10) {
                ASSERT_NO_FATAL_FAILURE(RefreshOwnershipChecksum(changed));
            }
        }
        ASSERT_NO_FATAL_FAILURE(WriteStagingFixture(capture.record, changed));
        ASSERT_NO_FATAL_FAILURE(SaveLedgerAfterStaging(save_dir.path()));
        EXPECT_TRUE(std::filesystem::is_regular_file(capture.record));
        EXPECT_EQ(ReadFileBytes(capture.record), changed);
        EXPECT_FALSE(std::filesystem::exists(capture.payload));
    }
}

TEST(WorldSaveService, LedgerStagingRepeatedPublicationInterruptionsDoNotAccumulateSidecars) {
    const TempSaveDir save_dir("repeated_published_sidecars");
    for (int interruption = 0; interruption < 8; ++interruption) {
        SCOPED_TRACE(interruption);
        ASSERT_NO_FATAL_FAILURE(LeavePublishedLedgerSidecar(save_dir.path()));
        const auto capture = CapturedLedgerStaging::current();
        ASSERT_NO_FATAL_FAILURE(SaveLedgerAfterStaging(save_dir.path(), interruption % 2 == 0));
        EXPECT_FALSE(std::filesystem::exists(capture.record));
        for (const auto& entry : std::filesystem::directory_iterator(save_dir.path()))
            EXPECT_FALSE(entry.path().filename().string().starts_with("active-regions.arl.tmp."));
    }
}

TEST(WorldSaveService, LedgerStagingReclaimsValidOrphanAfterPayloadRemoval) {
    const TempSaveDir save_dir("removed_ledger_payload");
    ASSERT_NO_FATAL_FAILURE(LeaveOwnedLedgerStaging(save_dir.path()));
    const auto capture = CapturedLedgerStaging::current();
    ASSERT_TRUE(std::filesystem::remove(capture.payload));
    ASSERT_NO_FATAL_FAILURE(SaveLedgerAfterStaging(save_dir.path(), true));
    EXPECT_FALSE(std::filesystem::exists(capture.record));
    EXPECT_FALSE(std::filesystem::exists(capture.payload));
    EXPECT_FALSE(std::filesystem::exists(WorldSaveService::active_regions_path(save_dir.path())));
}

TEST(WorldSaveService, LedgerStagingPreservesOrphanCopiedFromAnotherWorldOrInstance) {
    for (const bool foreign_world : {false, true}) {
        const TempSaveDir original("original_orphan_ownership");
        const TempSaveDir foreign("foreign_orphan_ownership");
        ASSERT_NO_FATAL_FAILURE(LeavePublishedLedgerSidecar(original.path()));
        const auto capture = CapturedLedgerStaging::current();
        const auto copied =
            foreign_world ? foreign.path() / capture.record.filename() : capture.record;
        if (!foreign_world)
            std::filesystem::rename(capture.record, original.path() / "held-record");
        ASSERT_NO_FATAL_FAILURE(WriteStagingFixture(copied, capture.ownership));
        ASSERT_NO_FATAL_FAILURE(
            SaveLedgerAfterStaging(foreign_world ? foreign.path() : original.path()));
        EXPECT_TRUE(std::filesystem::is_regular_file(copied));
        EXPECT_EQ(ReadFileBytes(copied), capture.ownership);
    }
}

TEST(WorldSaveService, LedgerStagingOwnershipSurvivesDirectoryAliases) {
    const TempSaveDir save_dir("ledger_directory_alias");
    const auto directory = save_dir.path() / Luminumbra::Filesystem::PathFromUtf8("world_\xc3\xa9");
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    ASSERT_NO_FATAL_FAILURE(LeaveOwnedLedgerStaging(directory));
    const auto capture = CapturedLedgerStaging::current();
    auto alias = directory / ".";
#if defined(_WIN32)
    const DWORD required = ::GetShortPathNameW(directory.c_str(), nullptr, 0);
    ASSERT_GT(required, 0u);
    // Bound the native path buffer to Windows' extended path limit.
    ASSERT_LE(required, 32768u);
    std::vector<wchar_t> short_name(required);
    const DWORD length = ::GetShortPathNameW(directory.c_str(), short_name.data(), required);
    ASSERT_GT(length, 0u);
    ASSERT_LT(length, required);
    alias = std::filesystem::path(short_name.data()) / ".";
#endif
    ASSERT_NO_FATAL_FAILURE(SaveLedgerAfterStaging(alias));
    EXPECT_FALSE(std::filesystem::exists(capture.payload));
    EXPECT_FALSE(std::filesystem::exists(capture.record));
}

TEST(WorldSaveService, LedgerStagingPreservesMalformedAndMismatchedOwnership) {
    // Magic, world identity, payload identity, record instance, name length,
    // filename, save-instance token and checksum each have independent coverage.
    for (const int mutation : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}) {
        SCOPED_TRACE(mutation);
        const TempSaveDir save_dir("invalid_ledger_ownership");
        ASSERT_NO_FATAL_FAILURE(LeaveOwnedLedgerStaging(save_dir.path()));
        const auto capture = CapturedLedgerStaging::current();
        auto changed = capture.ownership;
        ASSERT_GT(changed.size(), 152u);
        if (mutation == 0)
            changed.clear();
        else if (mutation == 1)
            changed.resize(changed.size() - 1);
        else if (mutation == 2)
            changed.assign(513, 'x');
        else {
            const std::array<std::size_t, 8> positions{
                0, 8, 32, 80, 104, 112, changed.size() - 9, changed.size() - 1};
            changed[positions[static_cast<std::size_t>(mutation - 3)]] ^= 1;
            if (mutation != 10) {
                ASSERT_NO_FATAL_FAILURE(RefreshOwnershipChecksum(changed));
            }
        }
        ASSERT_NO_FATAL_FAILURE(WriteStagingFixture(capture.record, changed));
        const auto payload = ReadFileBytes(capture.payload);
        ASSERT_NO_FATAL_FAILURE(SaveLedgerAfterStaging(save_dir.path()));
        EXPECT_TRUE(std::filesystem::is_regular_file(capture.payload));
        EXPECT_TRUE(std::filesystem::is_regular_file(capture.record));
        EXPECT_EQ(ReadFileBytes(capture.payload), payload);
        EXPECT_EQ(ReadFileBytes(capture.record), changed);
    }
}

TEST(WorldSaveService, LedgerStagingPreservesMissingOrSubstitutedProofAndPayload) {
    for (const int substitution : {0, 1, 2, 3, 4, 5, 6}) {
        SCOPED_TRACE(substitution);
        const TempSaveDir save_dir("substituted_ledger_staging");
        ASSERT_NO_FATAL_FAILURE(LeaveOwnedLedgerStaging(save_dir.path()));
        const auto capture = CapturedLedgerStaging::current();
        const auto payload = ReadFileBytes(capture.payload);
        if (substitution == 0) {
            ASSERT_TRUE(std::filesystem::remove(capture.record));
        } else if (substitution == 1) {
            // Same name and bytes, different file: identity is mandatory.
            ASSERT_TRUE(WorldSaveService::write_file_durably(capture.payload, payload));
        } else if (substitution == 2) {
            ASSERT_TRUE(WorldSaveService::write_file_durably(capture.record, capture.ownership));
        } else if (substitution == 3) {
            ASSERT_TRUE(std::filesystem::remove(capture.payload));
            ASSERT_TRUE(std::filesystem::create_directory(capture.payload));
            ASSERT_NO_FATAL_FAILURE(WriteStagingFixture(capture.payload / "keep", "keep"));
        } else if (substitution == 4) {
            std::filesystem::create_hard_link(capture.payload, save_dir.path() / "foreign-link");
        } else if (substitution == 5) {
            ASSERT_TRUE(std::filesystem::remove(capture.record));
            ASSERT_TRUE(std::filesystem::create_directory(capture.record));
            ASSERT_NO_FATAL_FAILURE(WriteStagingFixture(capture.record / "keep", "keep"));
        } else {
            std::filesystem::create_hard_link(capture.record, save_dir.path() / "foreign-link");
        }
        ASSERT_NO_FATAL_FAILURE(SaveLedgerAfterStaging(save_dir.path()));
        if (substitution == 3) {
            EXPECT_EQ(ReadFileBytes(capture.payload / "keep"), "keep");
        } else {
            EXPECT_EQ(ReadFileBytes(capture.payload), payload);
        }
        if (substitution == 0) {
            EXPECT_FALSE(std::filesystem::exists(capture.record));
        } else if (substitution == 5) {
            EXPECT_EQ(ReadFileBytes(capture.record / "keep"), "keep");
        } else {
            EXPECT_EQ(ReadFileBytes(capture.record), capture.ownership);
        }
    }
}

TEST(WorldSaveService, LedgerStagingPreservesPairCopiedFromAnotherWorldOrFilename) {
    for (const bool other_world : {false, true}) {
        const TempSaveDir original("original_ledger_staging");
        const TempSaveDir foreign("foreign_ledger_staging");
        ASSERT_NO_FATAL_FAILURE(LeaveOwnedLedgerStaging(original.path()));
        const auto capture = CapturedLedgerStaging::current();
        const auto directory = other_world ? foreign.path() : original.path();
        const auto payload =
            other_world
                ? directory / capture.payload.filename()
                : directory / "active-regions.arl.tmp.other.00000000000000000000000000000000";
        auto record = payload;
        record += ".owner";
        const auto expected = ReadFileBytes(capture.payload);
        std::filesystem::copy_file(capture.payload, payload);
        std::filesystem::copy_file(capture.record, record);
        ASSERT_NO_FATAL_FAILURE(SaveLedgerAfterStaging(directory));
        EXPECT_EQ(ReadFileBytes(payload), expected);
        EXPECT_EQ(ReadFileBytes(record), capture.ownership);
    }
}

#ifndef _WIN32
TEST(WorldSaveService, LedgerStagingReclaimsOrphansAfterDirectoryChangesAndReportsOccupiedLeaves) {
    // No timing or symlink privilege assumptions: POSIX exercises both absence
    // and occupied leaves, while the source contract covers the Windows open.
    for (const int occupant : {0, 1, 2, 3, 4, 5}) {
        SCOPED_TRACE(occupant);
        const TempSaveDir save_dir("changed_directory_ownership");
        ASSERT_NO_FATAL_FAILURE(LeavePublishedLedgerSidecar(save_dir.path()));
        const auto capture = CapturedLedgerStaging::current();
        const auto live_path = WorldSaveService::active_regions_path(save_dir.path());
        const auto published = ReadFileBytes(live_path);
        const auto entry = save_dir.path() / "unrelated-entry";
        const auto moved = save_dir.path() / "renamed-entry";
        ASSERT_NO_FATAL_FAILURE(WriteStagingFixture(entry, "directory change"));
        std::filesystem::rename(entry, moved);
        ASSERT_TRUE(std::filesystem::remove(moved));
        const auto target = save_dir.path() / "symlink-target";
        if (occupant == 1 || occupant == 2) {
            if (occupant == 2) {
                ASSERT_NO_FATAL_FAILURE(WriteStagingFixture(target, "foreign target"));
            }
            std::filesystem::create_symlink(target.filename(), capture.payload);
        } else if (occupant == 3) {
            ASSERT_TRUE(std::filesystem::create_directory(capture.payload));
        } else if (occupant == 4) {
            ASSERT_EQ(::mkfifo(capture.payload.c_str(), 0600), 0);
        } else if (occupant == 5) {
            ASSERT_NO_FATAL_FAILURE(WriteStagingFixture(capture.payload, "foreign payload"));
        }
        std::ostringstream messages;
        const auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(messages);
        const auto dispatcher = Log::GetDispatcherSink();
        dispatcher->Attach(sink);
        // Use the staging base to reclaim debris without deleting the live ledger.
        const bool removed =
            WorldSaveService::remove_file_and_staging(save_dir.path() / "active-regions.arl");
        dispatcher->Detach(sink);
        ASSERT_TRUE(removed);
        EXPECT_EQ(ReadFileBytes(live_path), published);
        if (occupant == 0) {
            EXPECT_FALSE(std::filesystem::exists(capture.record));
            EXPECT_EQ(std::filesystem::symlink_status(capture.payload).type(),
                      std::filesystem::file_type::not_found);
        } else {
            EXPECT_EQ(ReadFileBytes(capture.record), capture.ownership);
            EXPECT_NE(
                messages.str().find("preserving staging ownership: payload leaf is still present"),
                std::string::npos);
            if (occupant == 1 || occupant == 2) {
                EXPECT_TRUE(
                    std::filesystem::is_symlink(std::filesystem::symlink_status(capture.payload)));
                EXPECT_EQ(std::filesystem::read_symlink(capture.payload), target.filename());
                if (occupant == 1) {
                    EXPECT_FALSE(std::filesystem::exists(target));
                } else {
                    EXPECT_EQ(ReadFileBytes(target), "foreign target");
                }
            } else if (occupant == 3) {
                EXPECT_TRUE(std::filesystem::is_directory(capture.payload));
            } else if (occupant == 4) {
                EXPECT_EQ(std::filesystem::symlink_status(capture.payload).type(),
                          std::filesystem::file_type::fifo);
            } else {
                EXPECT_EQ(ReadFileBytes(capture.payload), "foreign payload");
            }
        }
    }
}

TEST(WorldSaveService, LedgerStagingPreservesSymlinkAndSpecialFileSubstitutions) {
    for (const int substitution : {0, 1, 2, 3}) {
        SCOPED_TRACE(substitution);
        const TempSaveDir save_dir("special_ledger_staging");
        ASSERT_NO_FATAL_FAILURE(LeaveOwnedLedgerStaging(save_dir.path()));
        const auto capture = CapturedLedgerStaging::current();
        const auto replaced = substitution % 2 == 0 ? capture.payload : capture.record;
        const auto target = save_dir.path() / "foreign-target";
        ASSERT_NO_FATAL_FAILURE(WriteStagingFixture(target, "foreign target"));
        ASSERT_TRUE(std::filesystem::remove(replaced));
        if (substitution < 2) {
            std::filesystem::create_symlink(target.filename(), replaced);
        } else {
            ASSERT_EQ(::mkfifo(replaced.c_str(), 0600), 0);
        }
        ASSERT_NO_FATAL_FAILURE(SaveLedgerAfterStaging(save_dir.path()));
        const auto kind = std::filesystem::symlink_status(replaced).type();
        EXPECT_EQ(kind,
                  substitution < 2 ? std::filesystem::file_type::symlink
                                   : std::filesystem::file_type::fifo);
        EXPECT_EQ(ReadFileBytes(target), "foreign target");
        if (replaced != capture.payload) {
            EXPECT_TRUE(std::filesystem::is_regular_file(capture.payload));
        }
        if (replaced != capture.record) {
            EXPECT_EQ(ReadFileBytes(capture.record), capture.ownership);
        }
    }
}
#endif

TEST(WorldSaveService, LedgerStagingSubstitutionBeforePublishUsesFreshName) {
    const TempSaveDir save_dir("ledger_publish_substitution");
    auto& capture = CapturedLedgerStaging::current();
    capture = {.directory = save_dir.path(), .payload = {}, .record = {}, .ownership = {}};
    WorldSaveService::set_before_active_regions_replace_for_testing([] {
        auto& staged = CapturedLedgerStaging::current();
        for (const auto& entry : std::filesystem::directory_iterator(staged.directory)) {
            if (entry.path().filename().string().starts_with("active-regions.arl.tmp.") &&
                entry.path().extension() != ".owner") {
                staged.payload = entry.path();
                staged.record = entry.path();
                staged.record += ".owner";
                staged.ownership = ReadFileBytes(staged.record);
                // Keep the original inode alive so replacement is deterministic.
                std::filesystem::rename(staged.payload, staged.directory / "held-ledger");
                WriteStagingFixture(staged.payload, "substituted payload");
                break;
            }
        }
        return true;
    });
    Luminumbra::world::ActiveRegionLedger ledger;
    ledger.set_local_anchor(Vec3(7, 8, 9));
    const bool saved = WorldSaveService::save_active_regions(
        ledger, Luminumbra::world::WorldClock{}, save_dir.path());
    WorldSaveService::set_before_active_regions_replace_for_testing(nullptr);
    ASSERT_TRUE(saved);
    ASSERT_FALSE(capture.payload.empty());
    EXPECT_EQ(ReadFileBytes(capture.payload), "substituted payload");
    EXPECT_EQ(ReadFileBytes(capture.record), capture.ownership);
    EXPECT_TRUE(std::filesystem::exists(save_dir.path() / "held-ledger"));
    EXPECT_EQ(ReadFileBytes(WorldSaveService::active_regions_path(save_dir.path())),
              ledger.encode());
}

#if defined(_WIN32)
TEST(WorldSaveService, LedgerStagingCaseAliasesPreserveUnknownOccupants) {
    const TempSaveDir save_dir("ledger_case_ownership");
    const auto payload = save_dir.path() / "ACTIVE-REGIONS.ARL.TMP.unknown";
    auto record = payload;
    record += ".OWNER";
    ASSERT_NO_FATAL_FAILURE(WriteStagingFixture(payload, "unknown payload"));
    ASSERT_NO_FATAL_FAILURE(WriteStagingFixture(record, "unknown ownership"));
    // An alias must take the ledger ownership branch, never generic temp deletion.
    ASSERT_TRUE(
        WorldSaveService::write_file_durably(save_dir.path() / "ACTIVE-REGIONS.ARL", "standalone"));
    EXPECT_EQ(ReadFileBytes(payload), "unknown payload");
    EXPECT_EQ(ReadFileBytes(record), "unknown ownership");
    ASSERT_NO_FATAL_FAILURE(SaveLedgerAfterStaging(save_dir.path()));
    EXPECT_EQ(ReadFileBytes(payload), "unknown payload");
    EXPECT_EQ(ReadFileBytes(record), "unknown ownership");
}

TEST(WorldSaveService, LedgerStagingPreservesUnreadableOrphanOwnershipUntilRetry) {
    const TempSaveDir save_dir("locked_orphan_ownership");
    ASSERT_NO_FATAL_FAILURE(LeavePublishedLedgerSidecar(save_dir.path()));
    const auto capture = CapturedLedgerStaging::current();
    const HANDLE locked = ::CreateFileW(capture.record.c_str(),
                                        GENERIC_READ,
                                        0,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL,
                                        nullptr);
    ASSERT_NE(locked, INVALID_HANDLE_VALUE);
    SaveLedgerAfterStaging(save_dir.path());
    EXPECT_NE(::CloseHandle(locked), 0);
    EXPECT_EQ(ReadFileBytes(capture.record), capture.ownership);
    ASSERT_NO_FATAL_FAILURE(SaveLedgerAfterStaging(save_dir.path()));
    EXPECT_FALSE(std::filesystem::exists(capture.record));
}

TEST(WorldSaveService, LedgerStagingPreservesUnreadableOwnership) {
    const TempSaveDir save_dir("locked_ledger_ownership");
    ASSERT_NO_FATAL_FAILURE(LeaveOwnedLedgerStaging(save_dir.path()));
    const auto capture = CapturedLedgerStaging::current();
    const HANDLE locked = ::CreateFileW(capture.record.c_str(),
                                        GENERIC_READ,
                                        0,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL,
                                        nullptr);
    ASSERT_NE(locked, INVALID_HANDLE_VALUE);
    SaveLedgerAfterStaging(save_dir.path());
    EXPECT_NE(::CloseHandle(locked), 0);
    EXPECT_TRUE(std::filesystem::is_regular_file(capture.payload));
    EXPECT_EQ(ReadFileBytes(capture.record), capture.ownership);
    ASSERT_NO_FATAL_FAILURE(SaveLedgerAfterStaging(save_dir.path()));
    EXPECT_FALSE(std::filesystem::exists(capture.payload));
    EXPECT_FALSE(std::filesystem::exists(capture.record));
}
#endif

TEST(WorldSaveService, LedgerStagingInterruptedWriteRemovesItsPairAndPreservesLiveLedger) {
    const TempSaveDir save_dir("ledger_rollback_ownership");
    ASSERT_NO_FATAL_FAILURE(SaveLedgerAfterStaging(save_dir.path()));
    const auto original = ReadFileBytes(WorldSaveService::active_regions_path(save_dir.path()));
    WorldSaveService::set_before_active_regions_replace_for_testing([] { return false; });
    Luminumbra::world::ActiveRegionLedger ledger;
    ledger.set_local_anchor(Vec3(7, 8, 9));
    const bool saved = WorldSaveService::save_active_regions(
        ledger, Luminumbra::world::WorldClock{}, save_dir.path());
    WorldSaveService::set_before_active_regions_replace_for_testing(nullptr);
    EXPECT_FALSE(saved);
    EXPECT_EQ(ReadFileBytes(WorldSaveService::active_regions_path(save_dir.path())), original);
    for (const auto& entry : std::filesystem::directory_iterator(save_dir.path()))
        EXPECT_FALSE(entry.path().filename().string().starts_with("active-regions.arl.tmp."));
}

#ifndef _WIN32
TEST(WorldSaveService, LedgerOwnershipFlushFailurePreservesLiveLedgerAndRemovesOwnPair) {
    const TempSaveDir save_dir("ledger_ownership_flush");
    const DirectoryFlushReset reset;
    const auto path = WorldSaveService::active_regions_path(save_dir.path());
    int staging_ancestors = 0;
    for (auto ancestor = save_dir.path(); !ancestor.empty() && ancestor != ancestor.root_path();
         ancestor = ancestor.parent_path())
        ++staging_ancestors;
    int destination_ancestors = 0;
    for (auto ancestor = path.parent_path(); !ancestor.empty() && ancestor != ancestor.root_path();
         ancestor = ancestor.parent_path())
        ++destination_ancestors;
    // Claim directory, promoted payload directory, completion directory, then
    // claim retirement all flush before destination publication begins.
    const int before_final_flush = staging_ancestors + 4 + destination_ancestors;
    for (const int failure : {0,
                              staging_ancestors,
                              staging_ancestors + 1,
                              staging_ancestors + 2,
                              staging_ancestors + 3,
                              staging_ancestors + 4,
                              before_final_flush}) {
        SCOPED_TRACE(failure);
        ASSERT_NO_FATAL_FAILURE(SaveLedgerAfterStaging(save_dir.path()));
        const auto original = ReadFileBytes(path);
        WorldSaveService::set_directory_flush_failure_for_testing(failure);
        Luminumbra::world::ActiveRegionLedger ledger;
        ledger.set_local_anchor(Vec3(7, 8, 9));
        EXPECT_FALSE(WorldSaveService::save_active_regions(
            ledger, Luminumbra::world::WorldClock{}, save_dir.path()));
        EXPECT_EQ(ReadFileBytes(path), failure == before_final_flush ? ledger.encode() : original);
        for (const auto& entry : std::filesystem::directory_iterator(save_dir.path()))
            EXPECT_FALSE(entry.path().filename().string().starts_with("active-regions.arl.tmp."));
        ASSERT_TRUE(WorldSaveService::validate_save(save_dir.path()));
    }
    ASSERT_NO_FATAL_FAILURE(SaveLedgerAfterStaging(save_dir.path()));
}
#endif

TEST(WorldSaveService, LoadFromEmptySaveDirIsCleanMissWithoutErrors) {
    const TempSaveDir save_dir("fresh");
    const WorldSaveService service;

    WorldStreamingState state;
    std::vector<std::string> errors;
    EXPECT_FALSE(service.load_world(state, save_dir.path(), errors));
    EXPECT_TRUE(errors.empty());
    EXPECT_TRUE(state.empty());
}

TEST(WorldSaveService, LoadFromMissingSaveDirIsCleanMissWithoutErrors) {
    const std::filesystem::path missing_dir =
        std::filesystem::temp_directory_path() / "luminumbra_world_save_service_does_not_exist";
    const WorldSaveService service;

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
    const TempSaveDir save_dir("dirty");
    const WorldSaveService service;

    WorldStreamingState state;
    PopulateFixtureWorld(state);
    auto edited = state.find_chunk(IVec3(0, 0, 0));
    ASSERT_NE(edited, nullptr);
    edited->sdf_data[1] = 9.5f;
    edited->mark_voxel_data_dirty();

    std::vector<std::string> errors;
    const auto report = service.save_dirty_chunks(state, save_dir.path(), &errors);
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(report.chunks_total, 3u);
    EXPECT_EQ(report.chunks_dirty, 1u);
    EXPECT_TRUE(report.saved);
    EXPECT_GE(report.regions_written, 1u);
    EXPECT_TRUE(std::filesystem::exists(WorldSaveService::region_file_path(save_dir.path(), 0, 0)));
    EXPECT_FALSE(edited->is_voxel_data_dirty());
    EXPECT_TRUE(state.dirty_chunk_ids().empty());

    // Second pass with nothing dirty performs no save.
    const auto clean_report = service.save_dirty_chunks(state, save_dir.path(), &errors);
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(clean_report.chunks_total, 3u);
    EXPECT_EQ(clean_report.chunks_dirty, 0u);
    EXPECT_FALSE(clean_report.saved);
}

TEST(ChunkDirtyTracking, SaveDirtyChunksWithoutDirtyChunksWritesNothing) {
    const TempSaveDir save_dir("clean");
    const WorldSaveService service;

    WorldStreamingState state;
    PopulateFixtureWorld(state);

    std::vector<std::string> errors;
    const auto report = service.save_dirty_chunks(state, save_dir.path(), &errors);
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(report.chunks_total, 3u);
    EXPECT_EQ(report.chunks_dirty, 0u);
    EXPECT_FALSE(report.saved);
    EXPECT_FALSE(std::filesystem::exists(WorldSaveService::world_state_path(save_dir.path())));
    EXPECT_FALSE(std::filesystem::exists(WorldSaveService::region_directory(save_dir.path())));
}

TEST(ChunkDirtyTracking, GenerationAndMeshingLeaveChunkClean) {
    Luminumbra::Systems::TerrainGenParams params = Luminumbra::Test::SyntheticTerrainParams();
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
    const TempSaveDir save_dir("clock_metadata");
    const auto metadata_path = save_dir.path() / "world_info.json";
    const auto read = [&]() {
        const std::ifstream file(metadata_path, std::ios::binary);
        std::ostringstream bytes;
        bytes << file.rdbuf();
        return bytes.str();
    };
    std::vector<std::string> errors;
    EXPECT_FALSE(WorldSaveService::save_metadata(
        R"({"container_version":2,"simulationTick":-1})", save_dir.path(), &errors));
    EXPECT_FALSE(std::filesystem::exists(metadata_path));
    EXPECT_FALSE(std::filesystem::exists(save_dir.path() / "chunks"));
    nlohmann::json metadata = {{"container_version", 2}};
    Luminumbra::world::WorldClock(123, {12000, 4}).write_metadata(metadata);
    // This fixture exercises indexed validation; clock metadata alone remains legacy.
    metadata["container_version"] = WorldSaveService::kIndexedMetadataVersion;
    metadata["snapshotFormat"] = 1;
    const auto valid = metadata.dump(4) + "\n";
    ASSERT_TRUE(WorldSaveService::save_metadata(valid, save_dir.path()));
    EXPECT_EQ(read(), valid);
    const auto index_path = Luminumbra::Persistence::snapshot_index_path(save_dir.path());
    ASSERT_TRUE(std::filesystem::exists(index_path));
    std::ifstream index_file(index_path, std::ios::binary);
    const std::string index_before(std::istreambuf_iterator<char>{index_file}, {});
    EXPECT_FALSE(WorldSaveService::save_metadata(R"({"container_version":2})", save_dir.path()));
    EXPECT_EQ(read(), valid);
    metadata["calendar"]["daysPerYear"] = 0;
    EXPECT_FALSE(WorldSaveService::save_metadata(metadata.dump(), save_dir.path()));
    EXPECT_EQ(read(), valid);
    // A pre-existing corrupt clock blocks chunk, entity and metadata writers.
    {
        std::ofstream output(metadata_path, std::ios::binary | std::ios::trunc);
        output << metadata.dump();
    }
    const auto damaged = read();
    WorldStreamingState state;
    PopulateFixtureWorld(state);
    EXPECT_FALSE(WorldSaveService{}.save_world(state, save_dir.path()));
    EXPECT_FALSE(WorldSaveService::save_metadata(valid, save_dir.path()));
    // This fixture uses an enabled indexed snapshot, so its region directory
    // already exists. Refusal must preserve the published index, not remove that directory.
    std::ifstream index_after_file(index_path, std::ios::binary);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>{index_after_file}, {}), index_before);
    EXPECT_EQ(read(), damaged);
    std::ofstream(metadata_path, std::ios::binary | std::ios::trunc) << "{";
    EXPECT_NO_THROW(EXPECT_FALSE(WorldSaveService::save_metadata(valid, save_dir.path(), &errors)));
    EXPECT_EQ(read(), "{");
}

TEST(WorldSaveService, LegacyMetadataWriterPreservesOpaqueBytesAndMissingDirectoryFailure) {
    const TempSaveDir root("legacy_metadata");
    const auto missing = root.path() / "missing";
    EXPECT_FALSE(WorldSaveService::save_metadata(R"({"container_version":2})", missing));
    EXPECT_FALSE(std::filesystem::exists(missing));
    for (const std::string bytes : {"{}", "{", "[]", "null", "opaque legacy input"}) {
        SCOPED_TRACE(bytes);
        const TempSaveDir destination("opaque_metadata");
        ASSERT_TRUE(WorldSaveService::save_metadata(bytes, destination.path()));
        const std::ifstream input(destination.path() / "world_info.json", std::ios::binary);
        std::ostringstream saved;
        saved << input.rdbuf();
        EXPECT_EQ(saved.str(), bytes);
    }
}

TEST(WorldSaveService, DirectLegacySnapshotsKeepLargeMetadata) {
    const TempSaveDir destination("large_metadata");
    const nlohmann::json metadata = {{"container_version", 2},
                                     {"extension", std::string(std::size_t{1024} * 1024u, 'x')}};
    const auto bytes = metadata.dump();
    ASSERT_TRUE(WorldSaveService::save_metadata(bytes, destination.path()));
    WorldStreamingState original, loaded;
    PopulateFixtureWorld(original);
    ASSERT_TRUE(WorldSaveService{}.save_world(original, destination.path()));
    std::vector<std::string> errors;
    ASSERT_TRUE(WorldSaveService{}.load_world(loaded, destination.path(), errors));
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(WorldSaveService{}.world_hash(loaded), WorldSaveService{}.world_hash(original));
    EXPECT_TRUE(WorldSaveService::save_metadata(bytes, destination.path()));
}

TEST(WorldSaveServiceTest, ChunkVectorRegionDigestUsesV2AuthorityAndDurableMerge) {
    using Luminumbra::Persistence::RegionDigestInputs;
    const TempSaveDir root("vector_digest");
    WorldStreamingState durable;
    const auto stored = AddFixtureChunk(durable, IVec3(0, 0, 0), ChunkState::Idle, 1u);
    const auto retained = AddFixtureChunk(durable, IVec3(1, 0, 0), ChunkState::Idle, 2u);
    stored->mark_voxel_data_dirty();
    retained->mark_voxel_data_dirty();
    ASSERT_TRUE(WorldSaveService{}.save_world(durable, root.path()));

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
    EXPECT_EQ(WorldSaveService::region_simulation_digest(root.path(), {0, 0}, chunks),
              expected_digest);
    EXPECT_EQ(WorldSaveService::region_simulation_digest(
                  root.path(), {0, 0}, RegionDigestInputs{chunks, {}, {}}),
              expected_digest);

    const std::vector<std::shared_ptr<Chunk>> empty;
    const auto durable_digest =
        std::stoull(Luminumbra::Persistence::ComputeRegionContentHash(durable), nullptr, 16);
    EXPECT_EQ(WorldSaveService::region_simulation_digest(root.path(), {0, 0}, empty),
              durable_digest);
    EXPECT_EQ(WorldSaveService::region_simulation_digest(root.path(), {0, 0}, {}), durable_digest);
    EXPECT_EQ(WorldSaveService::region_simulation_digest(root.path(), {0, 0}, {live}),
              expected_digest);
    const std::vector<std::shared_ptr<Chunk>> only_live{live};
    const auto live_digest = WorldSaveService::region_simulation_digest({}, {0, 0}, only_live);
    EXPECT_EQ(WorldSaveService::region_simulation_digest({}, {0, 0}, chunks), live_digest);
    EXPECT_EQ(WorldSaveService::region_simulation_digest(root.path() / "missing", {0, 0}, chunks),
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
    ASSERT_TRUE(WorldSaveService{}.save_world(resident, root.path()));

    std::vector<std::shared_ptr<Chunk>> saved;
    ASSERT_TRUE(WorldSaveService::read_authoritative_region_chunks(
        WorldSaveService::region_file_path(root.path(), key.x, key.z),
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
    const auto settled = freeze({}, RegionDigestInputs{resident.snapshot_chunks(), {}, {}});
    ASSERT_NE(settled, freeze({}, RegionDigestInputs{}));
    EXPECT_EQ(freeze(root.path(), RegionDigestInputs{resident.snapshot_chunks(), {}, {}}), settled);
    EXPECT_EQ(freeze({}, RegionDigestInputs{{}, resident.snapshot_chunks(), {}}), settled);

    // Incomplete live/parked content must fall back to the edited durable Loading record.
    auto incomplete = std::make_shared<Chunk>(chunk->get_coords());
    incomplete->heightmap_data = {99.0f};
    incomplete->mark_voxel_data_dirty();
    incomplete->set_state(ChunkState::Loading);
    EXPECT_EQ(freeze({}, RegionDigestInputs{{incomplete}, {}, {}}),
              freeze({}, RegionDigestInputs{}));
    EXPECT_EQ(freeze({}, RegionDigestInputs{{}, {incomplete}, {}}),
              freeze({}, RegionDigestInputs{}));
    EXPECT_EQ(freeze(root.path(), RegionDigestInputs{{incomplete}, {incomplete}, {}}), settled);

    resident.clear();
    chunk.reset();
    saved.clear();
    EXPECT_EQ(freeze(root.path(), RegionDigestInputs{}), settled);
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
                {}, {0, 0}, RegionDigestInputs{{chunk}, {}, {}});
            if (edited) {
                ASSERT_NE(settled, empty);
            } else {
                ASSERT_EQ(settled, empty);
            }
            chunk->set_state(lifecycle);
            const auto in_memory = lifecycle == ChunkState::Loading ? empty : settled;
            EXPECT_EQ(WorldSaveService::region_simulation_digest(
                          {}, {0, 0}, RegionDigestInputs{{chunk}, {}, {}}),
                      in_memory);
            EXPECT_EQ(WorldSaveService::region_simulation_digest(
                          {}, {0, 0}, RegionDigestInputs{{}, {chunk}, {}}),
                      in_memory);

            ASSERT_TRUE(WorldSaveService{}.save_world(resident, root.path()));
            WorldStreamingState loaded;
            std::vector<std::string> errors;
            ASSERT_TRUE(WorldSaveService{}.load_world(loaded, root.path(), errors));
            const auto saved = loaded.snapshot_chunks();
            ASSERT_EQ(saved.size(), 1u);
            EXPECT_EQ(saved.front()->get_state(), lifecycle);
            EXPECT_EQ(saved.front()->sdf_provenance(),
                      edited ? ChunkSdfProvenance::LoadedOrEdited
                             : ChunkSdfProvenance::GeneratedCurrentParams);
            // docs/active-regions.md: durable pristine records are regenerable caches.
            EXPECT_EQ(WorldSaveService::region_simulation_digest(root.path(), {0, 0}, {}), settled);
        }
    }
}

TEST(WorldSaveServiceTest, PublicRegionDigestRejectsUnsupportedKeysBeforeDurableRead) {
    using Luminumbra::world::RegionKey;
    const Luminumbra::Persistence::RegionDigestInputs inputs{};
    const TempSaveDir root("invalid_digest_key");
    const auto missing_save = root.path() / "missing";
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
        const Luminumbra::Persistence::RegionDigestInputs edited_inputs{{chunk}, {}, {}};
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

TEST(WorldSaveService, WaterCursorMetadataChecksTypeAndNativeSizeBeforeConversion) {
    const TempSaveDir save_dir("water_cursor_metadata");
    const auto path = save_dir.path() / "world_info.json";
    const auto read_cursor = [&](std::size_t& cursor) {
        Luminumbra::world::WorldClock clock;
        bool required = false;
        return WorldSaveService::read_clock_metadata(
            save_dir.path(), clock, required, nullptr, nullptr, nullptr, &cursor);
    };
    std::size_t cursor = 19;
    ASSERT_TRUE(read_cursor(cursor));
    EXPECT_EQ(cursor, 0u);
    for (const auto& bytes :
         {std::string(R"({"container_version":2})"),
          nlohmann::json{{"container_version", 2},
                         {"waterSimCursor", std::numeric_limits<std::size_t>::max()}}
              .dump()}) {
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << bytes;
            output.close();
            ASSERT_FALSE(output.fail());
        }
        cursor = 19;
        ASSERT_TRUE(read_cursor(cursor));
        EXPECT_EQ(cursor,
                  bytes.find("waterSimCursor") == std::string::npos
                      ? 0u
                      : std::numeric_limits<std::size_t>::max());
    }
    // Includes malformed JSON, a partial scalar, invalid types and a value
    // beyond uint64_t. On narrower size_t hosts, also refuse a uint64_t overflow.
    std::vector<std::string> invalid{"{",
                                     R"({"waterSimCursor":)",
                                     R"({"waterSimCursor":null})",
                                     R"({"waterSimCursor":-1})",
                                     R"({"waterSimCursor":1.5})",
                                     R"({"waterSimCursor":1e38})",
                                     R"({"waterSimCursor":true})",
                                     R"({"waterSimCursor":"0"})",
                                     R"({"waterSimCursor":18446744073709551616})"};
    if constexpr (std::numeric_limits<std::size_t>::max() <
                  std::numeric_limits<std::uint64_t>::max()) {
        invalid.push_back(
            nlohmann::json{{"waterSimCursor", std::numeric_limits<std::uint64_t>::max()}}.dump());
    }
    for (const auto& bytes : invalid) {
        SCOPED_TRACE(bytes);
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << bytes;
            output.close();
            ASSERT_FALSE(output.fail());
        }
        EXPECT_FALSE(read_cursor(cursor));
    }
    ASSERT_TRUE(std::filesystem::remove(path));
    ASSERT_TRUE(std::filesystem::create_directory(path));
    EXPECT_FALSE(read_cursor(cursor));
}

TEST(WorldSaveService, LedgerAdmissionStopsBeforeTheFileCapSoBothWritersStaySaveable) {
    using Luminumbra::world::ActiveRegionLedger;
    using Luminumbra::world::WorldClock;
    TempSaveDir save_dir("ledger_capacity");
    ActiveRegionLedger ledger;
    const auto key = [](std::uint32_t index) {
        return Luminumbra::world::RegionKey{static_cast<std::int32_t>(index % 1024u),
                                            static_cast<std::int32_t>(index / 1024u)};
    };
    // The age fits while one record is missing; the last record would outgrow the file cap.
    for (std::uint32_t index = 0; index + 1 < ActiveRegionLedger::kMaxRegions; ++index)
        ledger.mark_edited(key(index), WorldClock{});
    ASSERT_TRUE(ledger.set_energy_age({0, 0}, 0));
    const auto last = key(ActiveRegionLedger::kMaxRegions - 1);
    EXPECT_FALSE(ledger.can_activate(last));
    EXPECT_THROW(ledger.mark_edited(last, WorldClock{}), std::length_error);
    EXPECT_EQ(ledger.records().size(), ActiveRegionLedger::kMaxRegions - 1);

    std::vector<std::string> errors;
    EXPECT_TRUE(
        WorldSaveService::save_active_regions(ledger, WorldClock{}, save_dir.path(), &errors));
    EXPECT_TRUE(errors.empty());
    EXPECT_TRUE(std::filesystem::exists(WorldSaveService::active_regions_path(save_dir.path())));

    nlohmann::json metadata = {{"container_version", 2}};
    WorldClock{}.write_metadata(metadata);
    errors.clear();
    EXPECT_TRUE(WorldSaveService::save_metadata_and_active_regions(
        metadata.dump(), ledger, save_dir.path(), &errors));
    EXPECT_TRUE(errors.empty());
    EXPECT_TRUE(std::filesystem::exists(save_dir.path() / "world_info.json"));
}

TEST(WorldSaveService, BothCalendarKindsAreValidatedBeforeEveryWriter) {
    for (const auto& patch : Luminumbra::TestSupport::InvalidCalendarMetadata()) {
        SCOPED_TRACE(patch.dump());
        TempSaveDir save_dir("calendar_matrix");
        auto metadata = nlohmann::json{{"container_version", 2}};
        metadata.update(patch);
        EXPECT_FALSE(WorldSaveService::save_metadata(metadata.dump(), save_dir.path()));
        EXPECT_FALSE(std::filesystem::exists(save_dir.path() / "world_info.json"));
        Luminumbra::world::ActiveRegionLedger ledger;
        EXPECT_FALSE(WorldSaveService::save_metadata_and_active_regions(
            metadata.dump(), ledger, save_dir.path()));
        std::ofstream(save_dir.path() / "world_info.json", std::ios::binary) << metadata.dump();
        WorldStreamingState state;
        PopulateFixtureWorld(state);
        const auto before = metadata.dump();
        const auto read = [&]() {
            std::ifstream input(save_dir.path() / "world_info.json", std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(input), {});
        };
        EXPECT_FALSE(WorldSaveService{}.save_world(state, save_dir.path()));
        EXPECT_FALSE(WorldSaveService{}.save_dirty_chunks(state, save_dir.path()).saved);
        EXPECT_FALSE(WorldSaveService::save_plant_entities({}, save_dir.path()));
        EXPECT_FALSE(WorldSaveService::upsert_container_records(
            WorldSaveService::region_file_path(save_dir.path(), 0, 0), {}));
        EXPECT_FALSE(WorldSaveService::save_active_regions(
            ledger, Luminumbra::world::WorldClock{}, save_dir.path()));
        EXPECT_FALSE(
            WorldSaveService::save_metadata(R"({"container_version":2})", save_dir.path()));
        EXPECT_FALSE(WorldSaveService::save_metadata_and_active_regions(
            R"({"container_version":2,"simulationTick":0})", ledger, save_dir.path()));
        EXPECT_EQ(read(), before);
        EXPECT_FALSE(std::filesystem::exists(save_dir.path() / "chunks"));
    }
}

TEST(WorldSaveService, SupportedCalendarWritesNormalizeOnlyOnSuccessfulExplicitSave) {
    for (const auto* key : {"calendar", "renderCalendar"}) {
        TempSaveDir save_dir("calendar_normalize");
        const auto path = save_dir.path() / "world_info.json";
        auto metadata = nlohmann::json{{"container_version", 2}};
        metadata[key] = key == std::string("calendar")
                            ? nlohmann::json{{"dayLengthTicks", 36000}, {"daysPerYear", 3}}
                            : Luminumbra::TestSupport::DefaultCalendarMetadata();
        const std::string before = metadata.dump();
        std::ofstream(path, std::ios::binary) << before;
        const auto read = [&]() {
            std::ifstream input(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(input), {});
        };
        Luminumbra::world::WorldClock clock;
        bool required = false;
        ASSERT_TRUE(WorldSaveService::read_clock_metadata(save_dir.path(), clock, required));
        EXPECT_EQ(required, key == std::string("calendar"));
        EXPECT_EQ(read(), before);
        WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
        EXPECT_FALSE(WorldSaveService::save_metadata(before, save_dir.path()));
        EXPECT_EQ(read(), before);
        ASSERT_TRUE(WorldSaveService::save_metadata(before, save_dir.path()));
        const auto written = nlohmann::json::parse(read());
        EXPECT_EQ(written.at(key), metadata.at(key));
        if (required) {
            EXPECT_EQ(written.at(key).size(), 2u);
            EXPECT_FALSE(written.at(key).contains("version"));
            EXPECT_EQ(written.at(key).at("dayLengthTicks"), 36000);
            EXPECT_EQ(written.at(key).at("daysPerYear"), 3);
            // The two-field encoding still resolves the pinned solar/lunar values.
            ASSERT_TRUE(WorldSaveService::read_clock_metadata(save_dir.path(), clock, required));
            EXPECT_EQ(clock.calendar().daylightNumerator, 1u);
            EXPECT_EQ(clock.calendar().daylightDenominator, 2u);
            EXPECT_EQ(clock.calendar().lunarCycleDays, 8u);
        } else {
            EXPECT_EQ(written.at(key).at("version"), 2);
            EXPECT_EQ(written.at(key).size(), 6u);
            EXPECT_FALSE(written.contains("simulationTick"));
            EXPECT_FALSE(written.contains("calendar"));
        }
    }
}

#ifndef _WIN32
TEST(WorldSaveService, SavedInputReadRetriesInterruptionsForPayloadAndEofOnly) {
    using Luminumbra::Persistence::detail::ReadSavedInputRetryingInterrupt;
    for (const auto final_result : {ssize_t{0}, ssize_t{1}, ssize_t{-1}}) {
        int attempts = 0;
        const auto result = ReadSavedInputRetryingInterrupt([&] {
            ++attempts;
            if (attempts <= 2) {
                errno = EINTR;
                return ssize_t{-1};
            }
            errno = EIO;
            return final_result;
        });
        EXPECT_EQ(result, final_result);
        EXPECT_EQ(attempts, 3);
    }
}
TEST(WorldSaveService, SavedInputReadCancellationStopsAnInterruptedRetry) {
    using Luminumbra::Persistence::detail::ReadSavedInputRetryingInterrupt;
    std::stop_source cancellation;
    int attempts = 0;
    const auto result = ReadSavedInputRetryingInterrupt(
        [&] {
            ++attempts;
            cancellation.request_stop();
            errno = EINTR;
            return ssize_t{-1};
        },
        cancellation.get_token());
    EXPECT_EQ(result, -1);
    EXPECT_EQ(attempts, 1);
}
#endif

TEST(WorldSaveService, UnindexedCorruptClockRefusesWithoutCreatingChunkDirectory) {
    const TempSaveDir save_dir("unindexed_corrupt_clock");
    {
        std::ofstream output(save_dir.path() / "world_info.json", std::ios::binary);
        output << R"({"container_version":2,"simulationTick":-1})";
    }
    WorldStreamingState state;
    PopulateFixtureWorld(state);
    EXPECT_FALSE(WorldSaveService{}.save_world(state, save_dir.path()));
    EXPECT_FALSE(WorldSaveService::save_metadata(R"({"container_version":2})", save_dir.path()));
    EXPECT_FALSE(std::filesystem::exists(save_dir.path() / "chunks"));
}

namespace {
Luminumbra::Chunk*& ChunkChangedDuringDirtyPublication() {
    static Luminumbra::Chunk* chunk = nullptr;
    return chunk;
}
void EditChunkAfterRegionPublication(const char* step, const std::filesystem::path& path) {
    auto*& chunk = ChunkChangedDuringDirtyPublication();
    if (chunk && std::string_view(step) == "after-replace" && path.extension() == ".lmr") {
        chunk->sdf_data.front() += 1.0f;
        chunk->mark_voxel_data_dirty();
        chunk = nullptr;
    }
}
struct RemovalSubstitution {
    bool attempted = false;
    bool replaced = false;
};
RemovalSubstitution& RemovalSubstitutionState() {
    static RemovalSubstitution state;
    return state;
}
void SubstituteBeforeRemoval(const char* step, const std::filesystem::path& path) {
    auto& state = RemovalSubstitutionState();
    if (state.attempted || std::string_view(step) != "before-delete")
        return;
    state.attempted = true;
    std::error_code error;
    std::filesystem::rename(path, path.parent_path() / "retained-original", error);
    if (error)
        return;
    std::ofstream replacement(path, std::ios::binary);
    replacement << "replacement";
    replacement.close();
    state.replaced = replacement.good();
}
} // namespace

TEST(WorldSaveService, DurableRemovalRefusesSubstitutedOccupant) {
    const TempSaveDir save("remove_substitution");
    const auto path = save.path() / "world_info.json";
    {
        std::ofstream original(path, std::ios::binary);
        original << "original";
        original.close();
        ASSERT_TRUE(original.good());
    }
    RemovalSubstitutionState() = {};
    Luminumbra::Persistence::set_snapshot_step_hook_for_testing(&SubstituteBeforeRemoval);
    std::vector<std::string> errors;
    const bool removed = WorldSaveService::durable_remove(path, &errors);
    Luminumbra::Persistence::set_snapshot_step_hook_for_testing(nullptr);
    ASSERT_TRUE(RemovalSubstitutionState().attempted);
    ASSERT_TRUE(RemovalSubstitutionState().replaced);
    EXPECT_FALSE(removed);
    EXPECT_FALSE(errors.empty());
    std::ifstream replacement(path, std::ios::binary);
    std::string bytes;
    replacement >> bytes;
    EXPECT_EQ(bytes, "replacement");
    std::ifstream retained(save.path() / "retained-original", std::ios::binary);
    retained >> bytes;
    EXPECT_EQ(bytes, "original");
}

TEST(WorldSaveService, IndexOnlySnapshotRefusesUnlistedInvalidSingleton) {
    const TempSaveDir save("invalid_unlisted_singleton");
    Luminumbra::Persistence::SnapshotIndex index;
    index.generation = 1;
    index.txn_id = 1;
    const auto index_path = Luminumbra::Persistence::snapshot_index_path(save.path());
    std::filesystem::create_directories(index_path.parent_path());
    {
        std::ofstream output(index_path, std::ios::binary);
        output << Luminumbra::Persistence::encode_snapshot_index(index);
        output.close();
        ASSERT_TRUE(output.good());
    }
    WorldStreamingState state;
    std::vector<std::string> errors;
    EXPECT_FALSE(WorldSaveService{}.load_world(state, save.path(), errors));
    EXPECT_TRUE(errors.empty());
    const auto invalid = save.path() / "aether_state.efs";
    std::filesystem::create_directory(invalid);
    EXPECT_FALSE(WorldSaveService{}.load_world(state, save.path(), errors));
    EXPECT_FALSE(errors.empty());
#if !defined(_WIN32)
    ASSERT_TRUE(std::filesystem::remove(invalid));
    std::filesystem::create_symlink(save.path() / "missing", invalid);
    errors.clear();
    EXPECT_FALSE(WorldSaveService{}.load_world(state, save.path(), errors));
    EXPECT_FALSE(errors.empty());
    EXPECT_TRUE(std::filesystem::is_symlink(invalid));
#endif
}

TEST(WorldSaveService, SnapshotMarkerRequiresTheSupportedIntegerWithoutNarrowing) {
    const TempSaveDir save("snapshot_marker_admission");
    const std::vector<nlohmann::json> invalid = {
        nullptr, false, "1", -1, 0, 2, std::numeric_limits<std::uint64_t>::max()};
    for (const auto& marker : invalid) {
        SCOPED_TRACE(marker.dump());
        {
            std::ofstream output(save.path() / "world_info.json", std::ios::binary);
            output << nlohmann::json{{"container_version", 2}, {"snapshotFormat", marker}};
            output.close();
            ASSERT_TRUE(output.good());
        }
        Luminumbra::Persistence::MemberDigest digest;
        std::string error;
        EXPECT_FALSE(
            WorldSaveService::member_digest(save.path(), "world_info.json", digest, error));
        EXPECT_FALSE(error.empty());
        WorldStreamingState state;
        std::vector<std::string> errors;
        EXPECT_FALSE(WorldSaveService{}.load_world(state, save.path(), errors));
        EXPECT_FALSE(errors.empty());
        const auto exact = WorldSaveService::read_chunk(save.path(), {0, 0, 0});
        EXPECT_EQ(exact.status, WorldSaveService::ChunkReadStatus::Error);
    }
    for (const bool indexed_marker : {false, true}) {
        {
            nlohmann::json metadata = {{"container_version", 2}};
            if (indexed_marker)
                metadata["snapshotFormat"] = 1;
            std::ofstream output(save.path() / "world_info.json", std::ios::binary);
            output << metadata;
            output.close();
            ASSERT_TRUE(output.good());
        }
        Luminumbra::Persistence::MemberDigest digest;
        std::string error;
        EXPECT_TRUE(WorldSaveService::member_digest(save.path(), "world_info.json", digest, error));
        EXPECT_TRUE(error.empty());
    }
}

TEST(ChunkDirtyTracking, DirtySaveDoesNotAcknowledgeRevisionChangedDuringPublication) {
    const TempSaveDir save("dirty_revision_publication");
    WorldStreamingState state;
    const auto chunk = state.get_or_create_chunk({0, 0, 0});
    chunk->sdf_data.assign(17 * 17 * 17, -4.0f);
    chunk->mark_voxel_data_dirty();
    ChunkChangedDuringDirtyPublication() = chunk.get();
    Luminumbra::Persistence::set_snapshot_step_hook_for_testing(&EditChunkAfterRegionPublication);
    const auto report = WorldSaveService{}.save_dirty_chunks(state, save.path());
    Luminumbra::Persistence::set_snapshot_step_hook_for_testing(nullptr);
    ChunkChangedDuringDirtyPublication() = nullptr;
    EXPECT_TRUE(report.saved);
    EXPECT_TRUE(chunk->is_voxel_data_dirty());
    EXPECT_EQ(chunk->sdf_data.front(), -3.0f);
}

// ---- Ground object persistence ----

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

void ExpectGroundNotificationPayloadSave(const std::string& payload, bool rejected, bool joined) {
    using namespace Luminumbra::world;
    TempSaveDir save_dir("ground_notification_payload");
    GroundObjectStore store;
    ASSERT_TRUE(store.insert(MakeGroundObject(1, 100, 0)));
    const WorldClock clock;
    ActiveRegionLedger ledger;
    std::uint64_t id = 0;
    ASSERT_TRUE(ledger.allocate_persistent_id(id));
    ledger.mark_edited(GroundObjectStore::region_of(*store.find(id)), clock);
    nlohmann::json metadata = {{"container_version", 2}};
    clock.write_metadata(metadata);
    std::vector<std::string> errors;
    ASSERT_TRUE(WorldSaveService::save_metadata_and_active_regions(
        metadata.dump(), ledger, save_dir.path(), &errors))
        << JoinErrors(errors);
    luminumbra::simulation::OrderedEventBus bus;
    bus.publish(1, "ground.spawned", payload, -1);
    bus.publish(2, "ground.despawned", "kept", 2);
    bus.publish(3, "ground.despawned", payload);
    const GroundNotifications notifications{bus.next_sequence(), bus.pending_events()};
    const auto& expected = notifications;
    std::optional<Luminumbra::Persistence::SaveTransaction> transaction;
    if (joined) {
        transaction.emplace(save_dir.path(),
                            0,
                            std::vector<std::array<float, 3>>{},
                            WorldSaveService::member_digest);
        ASSERT_TRUE(transaction->begin(&errors)) << JoinErrors(errors);
    }
    bool saved = false;
    EXPECT_NO_THROW(saved = WorldSaveService::save_ground_objects(
                        store, save_dir.path(), &errors, notifications));
    if (rejected) {
        EXPECT_FALSE(saved);
        EXPECT_NE(JoinErrors(errors).find("2 pending notification(s) have non-UTF-8 payloads"),
                  std::string::npos);
        if (joined) {
            transaction->abort(&errors);
        }
        EXPECT_FALSE(
            std::filesystem::exists(WorldSaveService::ground_objects_path(save_dir.path())));
        EXPECT_EQ(bus.pending_count(), 3u);
        EXPECT_NO_THROW(static_cast<void>(notifications.canonical_bytes()));
        return;
    }
    ASSERT_TRUE(saved) << JoinErrors(errors);
    EXPECT_TRUE(errors.empty());
    if (joined) {
        ASSERT_TRUE(transaction->commit(&errors)) << JoinErrors(errors);
    }
    const auto bytes = ReadFileBytes(WorldSaveService::ground_objects_path(save_dir.path()));
    EXPECT_EQ(bytes, store.serialize(expected));
    const auto legacy = store.serialize();
    EXPECT_EQ(bytes,
              legacy.substr(0, legacy.size() - 2) +
                  ",\"notifications\":{\"next_sequence\":3,\"events\":[" +
                  notifications.events[0].canonical_bytes() + ',' +
                  notifications.events[1].canonical_bytes() + ',' +
                  notifications.events[2].canonical_bytes() + "]}}\n");
    std::string hash;
    EXPECT_NO_THROW(hash = notifications.canonical_bytes());
    EXPECT_EQ(hash, "ground_notifications:v1:" + GroundObjectStore{}.serialize(expected));
    GroundObjectStore restored;
    GroundNotifications loaded;
    ASSERT_TRUE(
        WorldSaveService::load_ground_objects(restored, save_dir.path(), 2, &errors, {}, &loaded))
        << JoinErrors(errors);
    EXPECT_EQ(restored.objects(), store.objects());
    EXPECT_EQ(loaded.next_sequence, 3u);
    EXPECT_EQ(restored.serialize(loaded), bytes);
    EXPECT_EQ(loaded.canonical_bytes(), hash);
    ASSERT_EQ(loaded.events.size(), 3u);
    EXPECT_EQ(loaded.events.front().payload, payload);
    EXPECT_EQ(loaded.events.back().payload, payload);
    EXPECT_EQ(bus.pending_count(), 3u);
    const auto delivered = bus.drain(3);
    ASSERT_EQ(delivered.size(), 3u);
    EXPECT_EQ(delivered.front().payload, payload);
    EXPECT_EQ(delivered.back().payload, payload);
}

void ExpectGroundNotificationSerializationRefusal(bool joined) {
    using namespace Luminumbra::world;
    TempSaveDir save_dir("ground_notification_exception");
    GroundNotifications notifications{1, {{0, 0, 0, "ground.spawned", "kept"}}};
    std::vector<std::string> errors;
    const WorldClock clock;
    ActiveRegionLedger ledger;
    nlohmann::json metadata = {{"container_version", 2}};
    clock.write_metadata(metadata);
    ASSERT_TRUE(WorldSaveService::save_metadata_and_active_regions(
        metadata.dump(), ledger, save_dir.path(), &errors))
        << JoinErrors(errors);
    ASSERT_TRUE(WorldSaveService::save_ground_objects({}, save_dir.path(), &errors, notifications));
    const auto path = WorldSaveService::ground_objects_path(save_dir.path());
    const auto bytes = ReadFileBytes(path);
    const auto index = ReadFileBytes(Luminumbra::Persistence::snapshot_index_path(save_dir.path()));
    std::optional<Luminumbra::Persistence::SaveTransaction> transaction;
    if (joined) {
        transaction.emplace(save_dir.path(),
                            0,
                            std::vector<std::array<float, 3>>{},
                            WorldSaveService::member_digest);
        ASSERT_TRUE(transaction->begin(&errors)) << JoinErrors(errors);
    }
    notifications.events.front().topic += static_cast<char>(0xff);
    std::string hash;
    EXPECT_NO_THROW(hash = notifications.canonical_bytes());
    EXPECT_EQ(hash, "!");
    bool saved = true;
    EXPECT_NO_THROW(
        saved = WorldSaveService::save_ground_objects({}, save_dir.path(), &errors, notifications));
    EXPECT_FALSE(saved);
    EXPECT_NE(JoinErrors(errors).find("serialization exception"), std::string::npos);
    EXPECT_EQ(ReadFileBytes(path), bytes);
    EXPECT_EQ(ReadFileBytes(Luminumbra::Persistence::snapshot_index_path(save_dir.path())), index);
}

void ExpectCombinedGroundSaveRefusal(std::uint64_t spawn_tick,
                                     bool record_region,
                                     std::string_view expected_error) {
    using namespace Luminumbra::world;
    SCOPED_TRACE(expected_error);
    TempSaveDir save_dir("ground_combined_precheck");
    GroundObjectStore store;
    ASSERT_TRUE(store.insert(MakeGroundObject(1, 100, spawn_tick)));
    const WorldClock clock;
    ActiveRegionLedger ledger;
    std::uint64_t id = 0;
    ASSERT_TRUE(ledger.allocate_persistent_id(id));
    if (record_region)
        ledger.mark_edited(GroundObjectStore::region_of(*store.find(id)), clock);
    nlohmann::json metadata = {{"container_version", 2}};
    clock.write_metadata(metadata);
    std::vector<std::string> errors;
    EXPECT_FALSE(WorldSaveService::save_metadata_active_regions_and_ground_objects(
        metadata.dump(), ledger, store, GroundNotifications{}, save_dir.path(), &errors));
    EXPECT_NE(JoinErrors(errors).find(expected_error), std::string::npos) << JoinErrors(errors);
    EXPECT_FALSE(std::filesystem::exists(WorldSaveService::ground_objects_path(save_dir.path())));
    EXPECT_FALSE(std::filesystem::exists(WorldSaveService::active_regions_path(save_dir.path())));
}

} // namespace

TEST(WorldSaveService, GroundNotificationInvalidUtf8PayloadsRefuseTheSaveWithoutWriting) {
    for (const bool joined : {false, true}) {
        SCOPED_TRACE(joined);
        ExpectGroundNotificationPayloadSave(std::string(1, static_cast<char>(0xff)), true, joined);
        ExpectGroundNotificationPayloadSave("\xe2\x82", true, joined);
    }
}

TEST(WorldSaveService, CombinedGroundSaveRefusesObjectsTheNextLoadWouldReject) {
    ExpectCombinedGroundSaveRefusal(0, false, "orphaned object");
    ExpectCombinedGroundSaveRefusal(5, true, "spawn tick is ahead of the clock");
}

TEST(WorldSaveService, GroundNotificationValidPayloadBytesAndHashSurviveSaveReload) {
    const auto payload =
        std::string("\0\b\f\n\r\t\"\\", 8) + "\xc3\xa9\xe2\x82\xac\xf0\x9f\x8c\x8d";
    for (const bool joined : {false, true})
        ExpectGroundNotificationPayloadSave(payload, false, joined);
}

TEST(WorldSaveService, GroundNotificationSerializationExceptionsRefuseBeforePublishing) {
    for (const bool joined : {false, true})
        ExpectGroundNotificationSerializationRefusal(joined);
}

TEST(WorldSaveService, NeverWrittenGroundObjectsStayUnlistedAcrossEmptySaves) {
    using Luminumbra::world::GroundObjectStore;
    TempSaveDir save_dir("ground_never_written");
    std::vector<std::string> errors;
    const Luminumbra::world::WorldClock clock;
    Luminumbra::world::ActiveRegionLedger ledger;
    nlohmann::json metadata = {{"container_version", WorldSaveService::kIndexedMetadataVersion},
                               {"snapshotFormat", 1}};
    clock.write_metadata(metadata);
    ASSERT_TRUE(WorldSaveService::save_metadata_and_active_regions(
        metadata.dump(), ledger, save_dir.path(), &errors))
        << JoinErrors(errors);
    Luminumbra::Persistence::SnapshotIndex initial;
    ASSERT_TRUE(Luminumbra::Persistence::read_snapshot_index(save_dir.path(), initial, &errors));
    ASSERT_TRUE(
        std::none_of(initial.members.begin(), initial.members.end(), [](const auto& member) {
            return member.path == GroundObjectStore::kRelativePath;
        }));
    const auto metadata_bytes = ReadFileBytes(save_dir.path() / "world_info.json");
    for (const bool joined : {false, true}) {
        SCOPED_TRACE(joined);
        {
            WorldSaveService::SaveFactsScope facts(save_dir.path());
            std::optional<Luminumbra::Persistence::SaveTransaction> transaction;
            if (joined) {
                transaction.emplace(save_dir.path(),
                                    clock.tick(),
                                    std::vector<std::array<float, 3>>{},
                                    WorldSaveService::member_digest);
                ASSERT_TRUE(transaction->begin(&errors)) << JoinErrors(errors);
            }
            ASSERT_TRUE(WorldSaveService::save_ground_objects({}, save_dir.path(), &errors))
                << JoinErrors(errors);
            ASSERT_TRUE(WorldSaveService::save_metadata_and_active_regions(
                metadata.dump(), ledger, save_dir.path(), &errors))
                << JoinErrors(errors);
            if (joined) {
                ASSERT_TRUE(transaction->commit(&errors)) << JoinErrors(errors);
            }
        }
        EXPECT_FALSE(
            std::filesystem::exists(WorldSaveService::ground_objects_path(save_dir.path())));
        EXPECT_EQ(ReadFileBytes(save_dir.path() / "world_info.json"), metadata_bytes);
        Luminumbra::Persistence::SnapshotIndex index;
        ASSERT_TRUE(Luminumbra::Persistence::read_snapshot_index(save_dir.path(), index, &errors));
        EXPECT_GT(index.generation, initial.generation);
        index.generation = initial.generation;
        index.txn_id = initial.txn_id;
        EXPECT_EQ(Luminumbra::Persistence::encode_snapshot_index(index),
                  Luminumbra::Persistence::encode_snapshot_index(initial));
        GroundObjectStore loaded;
        ASSERT_TRUE(loaded.insert(MakeGroundObject(1, 100, 0)));
        Luminumbra::world::GroundNotifications notifications{
            1, {{0, 0, 0, "ground.spawned", "stale"}}};
        ASSERT_TRUE(WorldSaveService::load_ground_objects(
            loaded, save_dir.path(), 2, &errors, {}, &notifications))
            << JoinErrors(errors);
        EXPECT_TRUE(loaded.empty());
        EXPECT_TRUE(notifications.events.empty());
        EXPECT_EQ(notifications.next_sequence, 0u);
        EXPECT_TRUE(WorldSaveService::validate_save(save_dir.path(), &errors))
            << JoinErrors(errors);
    }
    // An unlisted member is a clean miss only while its file is missing.
    {
        std::ofstream output(WorldSaveService::ground_objects_path(save_dir.path()),
                             std::ios::binary);
        output << GroundObjectStore{}.serialize({1, {{0, 0, 0, "ground.spawned", "unexpected"}}});
        ASSERT_TRUE(output.good());
    }
    GroundObjectStore loaded;
    EXPECT_FALSE(WorldSaveService::load_ground_objects(loaded, save_dir.path(), 2, &errors));
    EXPECT_FALSE(WorldSaveService::validate_save(save_dir.path(), &errors));
}

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
        WorldSaveService::SaveFactsScope facts(save_dir.path());
        Luminumbra::Persistence::SaveTransaction transaction(
            save_dir.path(), clock.tick(), {}, WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(&errors)) << JoinErrors(errors);
        ASSERT_TRUE(transaction.protect({GroundObjectStore::kRelativePath}, &errors))
            << JoinErrors(errors);
        ASSERT_TRUE(WorldSaveService::save_metadata_and_active_regions(
            metadata.dump(), ledger, save_dir.path(), &errors))
            << JoinErrors(errors);
        ASSERT_TRUE(WorldSaveService::save_ground_objects(store, save_dir.path(), &errors))
            << JoinErrors(errors);
        ASSERT_TRUE(transaction.commit(&errors)) << JoinErrors(errors);
    }
    EXPECT_TRUE(fs::exists(WorldSaveService::ground_objects_path(save_dir.path())));

    GroundObjectStore loaded;
    ASSERT_TRUE(WorldSaveService::load_ground_objects(loaded, save_dir.path(), 100, &errors))
        << JoinErrors(errors);
    EXPECT_EQ(loaded.objects(), store.objects());

    ASSERT_TRUE(
        WorldSaveService::save_ground_objects(GroundObjectStore{}, save_dir.path(), &errors));
    EXPECT_FALSE(fs::exists(WorldSaveService::ground_objects_path(save_dir.path())));
    GroundObjectStore absent;
    ASSERT_TRUE(WorldSaveService::load_ground_objects(absent, save_dir.path(), 100, &errors));
    EXPECT_TRUE(absent.empty());
    // Repeating an empty save still declares the absent member in the snapshot.
    ASSERT_TRUE(WorldSaveService::save_ground_objects(absent, save_dir.path(), &errors))
        << JoinErrors(errors);
    Luminumbra::Persistence::SnapshotIndex index;
    ASSERT_TRUE(Luminumbra::Persistence::read_snapshot_index(save_dir.path(), index, &errors));
    const auto member =
        std::find_if(index.members.begin(), index.members.end(), [](const auto& value) {
            return value.path == GroundObjectStore::kRelativePath;
        });
    ASSERT_NE(member, index.members.end());
    EXPECT_FALSE(member->present);
    EXPECT_EQ(member->bytes, 0u);
    EXPECT_EQ(member->fnv64, 0u);
}

TEST(WorldSaveService, GroundObjectsJoinedEmptySaveRemovesFileAndRetainsAbsentMember) {
    using Luminumbra::world::GroundObjectStore;
    TempSaveDir save_dir("ground_joined_empty");
    GroundObjectStore store;
    ASSERT_TRUE(store.insert(MakeGroundObject(1, 100, 0)));
    std::vector<std::string> errors;
    // Joined writers validate the installed snapshot, including object ownership.
    const Luminumbra::world::WorldClock clock;
    Luminumbra::world::ActiveRegionLedger ledger;
    std::uint64_t id = 0;
    ASSERT_TRUE(ledger.allocate_persistent_id(id));
    ledger.mark_edited(GroundObjectStore::region_of(*store.find(id)), clock);
    nlohmann::json metadata = {{"container_version", 2}};
    clock.write_metadata(metadata);
    ASSERT_TRUE(WorldSaveService::save_metadata_and_active_regions(
        metadata.dump(), ledger, save_dir.path(), &errors))
        << JoinErrors(errors);
    ASSERT_TRUE(WorldSaveService::save_ground_objects(store, save_dir.path(), &errors))
        << JoinErrors(errors);
    // Empty objects still need a present member for pending events or a sequence cursor.
    for (const bool queued : {true, false}) {
        SCOPED_TRACE(queued);
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        Luminumbra::world::GroundNotifications notifications{maximum, {}};
        if (queued) {
            notifications.events.push_back(
                {0, 0, maximum - 1, "ground.despawned", "1,3,100,1000,-2000"});
        }
        ASSERT_TRUE(
            WorldSaveService::save_ground_objects({}, save_dir.path(), &errors, notifications))
            << JoinErrors(errors);
        Luminumbra::Persistence::SnapshotIndex index;
        ASSERT_TRUE(Luminumbra::Persistence::read_snapshot_index(save_dir.path(), index, &errors));
        const auto member =
            std::find_if(index.members.begin(), index.members.end(), [](const auto& value) {
                return value.path == GroundObjectStore::kRelativePath;
            });
        ASSERT_NE(member, index.members.end());
        EXPECT_TRUE(member->present);
        GroundObjectStore loaded;
        Luminumbra::world::GroundNotifications restored;
        ASSERT_TRUE(WorldSaveService::load_ground_objects(
            loaded, save_dir.path(), 2, &errors, {}, &restored))
            << JoinErrors(errors);
        EXPECT_TRUE(loaded.empty());
        EXPECT_EQ(loaded.serialize(restored), GroundObjectStore{}.serialize(notifications));
        EXPECT_TRUE(WorldSaveService::validate_save(save_dir.path(), &errors))
            << JoinErrors(errors);
    }
    std::uint64_t removed_generation = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        SCOPED_TRACE(attempt);
        {
            WorldSaveService::SaveFactsScope facts(save_dir.path());
            Luminumbra::Persistence::SaveTransaction transaction(
                save_dir.path(), 0, {}, WorldSaveService::member_digest);
            ASSERT_TRUE(transaction.begin(&errors)) << JoinErrors(errors);
            ASSERT_TRUE(transaction.protect({GroundObjectStore::kRelativePath}, &errors))
                << JoinErrors(errors);
            ASSERT_TRUE(WorldSaveService::save_ground_objects({}, save_dir.path(), &errors))
                << JoinErrors(errors);
            ASSERT_TRUE(transaction.commit(&errors)) << JoinErrors(errors);
        }
        EXPECT_FALSE(
            std::filesystem::exists(WorldSaveService::ground_objects_path(save_dir.path())));
        Luminumbra::Persistence::SnapshotIndex index;
        ASSERT_TRUE(Luminumbra::Persistence::read_snapshot_index(save_dir.path(), index, &errors));
        const auto member =
            std::find_if(index.members.begin(), index.members.end(), [](const auto& value) {
                return value.path == GroundObjectStore::kRelativePath;
            });
        ASSERT_NE(member, index.members.end());
        EXPECT_FALSE(member->present);
        EXPECT_EQ(member->bytes, 0u);
        EXPECT_EQ(member->fnv64, 0u);
        if (attempt == 0) {
            removed_generation = member->generation;
        } else {
            EXPECT_EQ(member->generation, removed_generation);
        }
        GroundObjectStore loaded;
        ASSERT_TRUE(WorldSaveService::load_ground_objects(loaded, save_dir.path(), 2, &errors))
            << JoinErrors(errors);
        EXPECT_TRUE(loaded.empty());
        EXPECT_TRUE(WorldSaveService::validate_save(save_dir.path(), &errors))
            << JoinErrors(errors);
    }
    // A listed absent member must also refuse a reintroduced file.
    {
        std::ofstream output(WorldSaveService::ground_objects_path(save_dir.path()),
                             std::ios::binary);
        output << store.serialize();
        ASSERT_TRUE(output.good());
    }
    GroundObjectStore loaded;
    EXPECT_FALSE(WorldSaveService::load_ground_objects(loaded, save_dir.path(), 2, &errors));
    EXPECT_FALSE(WorldSaveService::validate_save(save_dir.path(), &errors));
}

TEST(WorldSaveService, GroundNotificationGrowthBudgetRefusesBeforePublishing) {
    using Luminumbra::world::GroundObjectStore;
    TempSaveDir save_dir("ground_notification_capacity");
    GroundObjectStore store;
    ASSERT_TRUE(store.insert(MakeGroundObject(1, 100, 0)));
    std::vector<std::string> errors;
    ASSERT_TRUE(WorldSaveService::save_ground_objects(store, save_dir.path(), &errors));
    const auto path = WorldSaveService::ground_objects_path(save_dir.path());
    const auto before = ReadFileBytes(path);
    Luminumbra::world::GroundNotifications notifications{1, {{0, 0, 0, "ground.spawned", ""}}};
    notifications.events.front().payload.assign(
        GroundObjectStore::kMaxFileBytes - store.serialize(notifications).size(), 'x');
    ASSERT_EQ(store.serialize(notifications).size(), GroundObjectStore::kMaxFileBytes);
    EXPECT_FALSE(
        WorldSaveService::save_ground_objects(store, save_dir.path(), &errors, notifications));
    EXPECT_FALSE(errors.empty());
    EXPECT_EQ(ReadFileBytes(path), before);
}

TEST(WorldSaveService, GroundObjectIdAtOrAboveNextIdIsRefused) {
    using Luminumbra::world::GroundObjectStore;
    TempSaveDir save_dir("ground_next_id");
    GroundObjectStore store;
    ASSERT_TRUE(store.insert(MakeGroundObject(5, 100, 0)));
    std::vector<std::string> errors;
    ASSERT_TRUE(WorldSaveService::save_ground_objects(store, save_dir.path(), &errors));

    GroundObjectStore loaded;
    EXPECT_FALSE(WorldSaveService::load_ground_objects(loaded, save_dir.path(), 5, &errors));
    EXPECT_TRUE(loaded.empty()) << "a refused load leaves the output untouched";
    EXPECT_TRUE(WorldSaveService::load_ground_objects(loaded, save_dir.path(), 6, &errors));
    EXPECT_EQ(loaded.size(), 1u);
}

TEST(WorldSaveService, CorruptGroundObjectsFileIsRefusedByLoadAndMemberDigest) {
    TempSaveDir save_dir("ground_corrupt");
    const auto path = WorldSaveService::ground_objects_path(save_dir.path());
    std::filesystem::create_directories(path.parent_path());
    {
        std::ofstream out(path, std::ios::binary);
        out << "{\"schema\":\"luminumbra.ground_objects.v1\",\"objects\":[";
    }
    Luminumbra::world::GroundObjectStore loaded;
    std::vector<std::string> errors;
    EXPECT_FALSE(WorldSaveService::load_ground_objects(loaded, save_dir.path(), 100, &errors));
    EXPECT_FALSE(errors.empty());

    Luminumbra::Persistence::MemberDigest digest;
    std::string error;
    EXPECT_FALSE(WorldSaveService::member_digest(
        save_dir.path(), "chunks/region/ground-objects.json", digest, error));
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
    ASSERT_TRUE(service.save_world(original, save_dir.path(), &save_errors))
        << JoinErrors(save_errors);

    Luminumbra::world::GroundObjectStore store;
    ASSERT_TRUE(store.insert(MakeGroundObject(1, 100, 0)));
    ASSERT_TRUE(WorldSaveService::save_ground_objects(store, save_dir.path(), &save_errors))
        << JoinErrors(save_errors);

    WorldStreamingState restored;
    std::vector<std::string> load_errors;
    EXPECT_FALSE(service.load_world(restored, save_dir.path(), load_errors));
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
        .live = {}, .parked = {}, .durable = {}, .ground_object_bytes = {}};
    first.ground_object_bytes = "1,3,100,1000,-2000,0,0,0,7,0;";
    Luminumbra::Persistence::RegionDigestInputs second{
        .live = {}, .parked = {}, .durable = {}, .ground_object_bytes = {}};
    second.ground_object_bytes = "1,3,101,1000,-2000,0,0,0,7,0;";
    const std::uint64_t with_first = WorldSaveService::region_simulation_digest(no_dir, key, first);
    const std::uint64_t with_second =
        WorldSaveService::region_simulation_digest(no_dir, key, second);
    EXPECT_NE(with_first, plain);
    EXPECT_NE(with_second, plain);
    EXPECT_NE(with_first, with_second);
}

namespace {
using Luminumbra::world::FieldPageState;
enum class FieldSection {
    Pages,
    Storm,
    Strike,
    Scent,
    Clock,
    Soil,
    Irrigation
};

void AddFieldSection(FieldPageState& state, FieldSection section) {
    switch (section) {
        case FieldSection::Pages:
            EXPECT_TRUE(state.activate_region({0, 0}, 42, 16));
            EXPECT_TRUE(state.activate_region({1, 0}, 42, 16));
            break;
        case FieldSection::Storm: {
            Luminumbra::world::RegionalStorm storm;
            storm.owner = {1, 0};
            storm.center_x = std::bit_cast<std::uint32_t>(512.0f);
            storm.lifetime_ticks = 100;
            EXPECT_TRUE(state.replace_weather({storm}, {}));
            break;
        }
        case FieldSection::Strike: {
            Luminumbra::world::RegionalStrike strike;
            strike.owner = {1, 0};
            strike.world_x = std::bit_cast<std::uint32_t>(512.0f);
            EXPECT_TRUE(state.replace_weather(state.storms(), {strike}));
            break;
        }
        case FieldSection::Scent:
            EXPECT_TRUE(state.set_scent({512, 0}, {std::bit_cast<std::uint64_t>(1.0), 0, 0, 0}));
            break;
        case FieldSection::Clock:
            EXPECT_TRUE(state.set_weather_age({1, 0}, 13));
            break;
        case FieldSection::Soil:
            EXPECT_TRUE(state.set_soil({512, 0}, 999997));
            break;
        case FieldSection::Irrigation:
            EXPECT_TRUE(state.set_irrigation({512, 0}, 3999999));
            break;
    }
}

void ExpectFieldMember(const std::filesystem::path& save_dir, const std::string& bytes) {
    Luminumbra::Persistence::SnapshotIndex index;
    ASSERT_TRUE(Luminumbra::Persistence::read_snapshot_index(save_dir, index, nullptr));
    const auto member = std::find_if(index.members.begin(), index.members.end(), [](const auto& m) {
        return m.path == FieldPageState::kRelativePath;
    });
    ASSERT_NE(member, index.members.end());
    EXPECT_EQ(member->present, !bytes.empty());
    EXPECT_EQ(member->bytes, bytes.size());
    EXPECT_EQ(member->fnv64,
              bytes.empty() ? 0
                            : Luminumbra::Persistence::fnv1a64_bytes(bytes.data(), bytes.size()));
}

void ExpectFieldRoundTrip() {
    const TempSaveDir save_dir("field_roundtrip");
    FieldPageState state;
    for (const auto section : {FieldSection::Pages,
                               FieldSection::Storm,
                               FieldSection::Strike,
                               FieldSection::Scent,
                               FieldSection::Clock,
                               FieldSection::Soil,
                               FieldSection::Irrigation}) {
        AddFieldSection(state, section);
    }
    const auto bytes = state.serialize();
    ASSERT_FALSE(bytes.empty());
    const Luminumbra::world::WorldClock clock;
    Luminumbra::world::ActiveRegionLedger ledger;
    ledger.mark_edited({0, 0}, clock);
    ledger.mark_edited({1, 0}, clock);
    nlohmann::json metadata = {{"container_version", 2}};
    clock.write_metadata(metadata);
    std::vector<std::string> errors;
    {
        WorldSaveService::SaveFactsScope facts(save_dir.path());
        Luminumbra::Persistence::SaveTransaction transaction(
            save_dir.path(), 0, {}, WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(&errors)) << JoinErrors(errors);
        ASSERT_TRUE(WorldSaveService::save_metadata_and_active_regions(
            metadata.dump(), ledger, save_dir.path(), &errors))
            << JoinErrors(errors);
        WorldSaveService::PreparedFieldWrite prepared;
        ASSERT_TRUE(prepared.prepare(state, &errors)) << JoinErrors(errors);
        state.clear();
        ASSERT_TRUE(prepared.publish(save_dir.path(), &errors)) << JoinErrors(errors);
        FieldPageState staged;
        ASSERT_TRUE(WorldSaveService::load_field_pages(staged, save_dir.path(), &errors));
        EXPECT_EQ(staged.serialize(), bytes);
        ASSERT_TRUE(WorldSaveService::finalize_indexed_envelope(save_dir.path(), &errors));
        ASSERT_TRUE(transaction.commit(&errors)) << JoinErrors(errors);
    }
    EXPECT_EQ(ReadFileBytes(WorldSaveService::field_pages_path(save_dir.path())), bytes);
    ExpectFieldMember(save_dir.path(), bytes);
    FieldPageState loaded;
    ASSERT_TRUE(WorldSaveService::load_field_pages(loaded, save_dir.path(), &errors));
    EXPECT_EQ(loaded.serialize(), bytes);
    WorldStreamingState restored;
    EXPECT_TRUE(WorldSaveService{}.load_world(restored, save_dir.path(), errors))
        << JoinErrors(errors);
    ASSERT_TRUE(WorldSaveService::save_field_pages(loaded, save_dir.path(), &errors))
        << JoinErrors(errors);
    {
        Luminumbra::Persistence::SaveTransaction transaction(
            save_dir.path(), 0, {}, WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(&errors)) << JoinErrors(errors);
        ASSERT_TRUE(WorldSaveService::save_field_pages({}, save_dir.path(), &errors));
        EXPECT_FALSE(std::filesystem::exists(WorldSaveService::field_pages_path(save_dir.path())));
        ASSERT_TRUE(WorldSaveService::load_field_pages(loaded, save_dir.path(), &errors));
        EXPECT_TRUE(loaded.empty());
        transaction.abort(&errors);
    }
    EXPECT_EQ(ReadFileBytes(WorldSaveService::field_pages_path(save_dir.path())), bytes);
    ExpectFieldMember(save_dir.path(), bytes);
    ASSERT_TRUE(WorldSaveService::load_field_pages(loaded, save_dir.path(), &errors));
    EXPECT_EQ(loaded.serialize(), bytes);
    ASSERT_TRUE(WorldSaveService::save_field_pages({}, save_dir.path(), &errors))
        << JoinErrors(errors);
    EXPECT_FALSE(std::filesystem::exists(WorldSaveService::field_pages_path(save_dir.path())));
    ASSERT_TRUE(WorldSaveService::load_field_pages(loaded, save_dir.path(), &errors));
    EXPECT_TRUE(loaded.empty());
    ExpectFieldMember(save_dir.path(), "");
    ASSERT_TRUE(WorldSaveService::save_field_pages({}, save_dir.path(), &errors));
    ExpectFieldMember(save_dir.path(), "");
}

void ExpectBadFieldMember(bool oversized) {
    const TempSaveDir save_dir(oversized ? "field_oversized" : "field_corrupt");
    WorldStreamingState empty;
    ASSERT_TRUE(WorldSaveService{}.save_world(empty, save_dir.path()));
    const auto path = WorldSaveService::field_pages_path(save_dir.path());
    ASSERT_TRUE(WorldSaveService::write_file_durably(path, "FPG1"));
    if (oversized) {
        std::filesystem::resize_file(path, FieldPageState::kMaxFileBytes + 1);
    }
    std::vector<std::string> errors;
    Luminumbra::Persistence::SaveTransaction transaction(
        save_dir.path(), 0, {}, WorldSaveService::member_digest);
    ASSERT_TRUE(transaction.begin(&errors)) << JoinErrors(errors);
    FieldPageState loaded;
    AddFieldSection(loaded, FieldSection::Pages);
    const auto before = loaded.serialize();
    EXPECT_FALSE(WorldSaveService::load_field_pages(loaded, save_dir.path(), &errors));
    EXPECT_EQ(loaded.serialize(), before);
    EXPECT_NE(JoinErrors(errors).find(oversized ? "oversized file (64 MiB cap)"
                                                : "Field pages load failed: field pages:"),
              std::string::npos)
        << JoinErrors(errors);
    Luminumbra::Persistence::MemberDigest digest;
    std::string error;
    EXPECT_FALSE(WorldSaveService::member_digest(
        save_dir.path(), FieldPageState::kRelativePath, digest, error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(WorldSaveService{}.load_world(empty, save_dir.path(), errors));
}

void ExpectOrphanFieldSection(FieldSection section, bool with_ledger) {
    const TempSaveDir save_dir("field_orphan");
    WorldStreamingState empty;
    ASSERT_TRUE(WorldSaveService{}.save_world(empty, save_dir.path()));
    Luminumbra::world::ActiveRegionLedger ledger;
    const Luminumbra::world::WorldClock clock;
    ledger.mark_edited({0, 0}, clock);
    std::vector<std::string> errors;
    EXPECT_TRUE(WorldSaveService::validate_field_pages(save_dir.path(), ledger, &errors));
    if (with_ledger) {
        ASSERT_TRUE(WorldSaveService::save_active_regions(ledger, clock, save_dir.path(), &errors));
    }
    FieldPageState state;
    AddFieldSection(state, section);
    {
        Luminumbra::Persistence::SaveTransaction transaction(
            save_dir.path(), 0, {}, WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(&errors)) << JoinErrors(errors);
        ASSERT_TRUE(WorldSaveService::save_field_pages(state, save_dir.path(), &errors))
            << JoinErrors(errors);
        ASSERT_TRUE(WorldSaveService::finalize_indexed_envelope(save_dir.path(), &errors));
        ASSERT_TRUE(transaction.commit(&errors)) << JoinErrors(errors);
    }
    EXPECT_FALSE(WorldSaveService::validate_field_pages(save_dir.path(), ledger, &errors));
    EXPECT_NE(JoinErrors(errors).find("corrupt field pages: orphaned owner"), std::string::npos);
    errors.clear();
    EXPECT_FALSE(WorldSaveService{}.load_world(empty, save_dir.path(), errors));
    EXPECT_NE(JoinErrors(errors).find("corrupt field pages: orphaned owner"), std::string::npos);
    if (with_ledger) {
        ledger.mark_edited({1, 0}, clock);
        errors.clear();
        EXPECT_TRUE(WorldSaveService::validate_field_pages(save_dir.path(), ledger, &errors))
            << JoinErrors(errors);
    }
}

void ExpectFieldDigest(bool ground_present, bool pages_present) {
    using Luminumbra::Persistence::RegionDigestInputs;
    using Luminumbra::Persistence::StableChecksum;
    FieldPageState state;
    AddFieldSection(state, FieldSection::Pages);
    RegionDigestInputs inputs;
    if (ground_present)
        inputs.ground_object_bytes = "ground projection";
    if (pages_present)
        inputs.field_page_bytes = state.region_bytes({0, 0});
    WorldStreamingState empty;
    std::string expected = Luminumbra::Persistence::ComputeRegionContentHash(empty);
    if (ground_present)
        expected = StableChecksum(expected + "|ground:" + inputs.ground_object_bytes);
    if (pages_present)
        expected = StableChecksum(expected + "|pages:" + inputs.field_page_bytes);
    const TempSaveDir save_dir("field_digest");
    {
        Luminumbra::Persistence::SaveTransaction transaction(
            save_dir.path(), 0, {}, WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(WorldSaveService::finalize_indexed_envelope(save_dir.path()));
        ASSERT_TRUE(transaction.commit(nullptr));
    }
    Luminumbra::Persistence::RegionDigestOperation operation(save_dir.path(), true);
    const auto value = std::stoull(expected, nullptr, 16);
    EXPECT_EQ(operation.digest({0, 0}, inputs), value);
    EXPECT_EQ(WorldSaveService::region_simulation_digest({}, {0, 0}, inputs), value);
    inputs.field_page_bytes += "changed";
    EXPECT_NE(operation.digest({0, 0}, inputs), value);
    EXPECT_EQ(operation.index_reads(), 1u);
}

void ExpectUnlistedFieldMemberRefused() {
    const TempSaveDir save_dir("field_unlisted");
    Luminumbra::world::ActiveRegionLedger ledger;
    const Luminumbra::world::WorldClock clock;
    ledger.mark_edited({0, 0}, clock);
    ledger.mark_edited({1, 0}, clock);
    ASSERT_TRUE(WorldSaveService::save_active_regions(ledger, clock, save_dir.path()));
    {
        Luminumbra::Persistence::SaveTransaction transaction(
            save_dir.path(), 0, {}, WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(WorldSaveService::save_field_pages({}, save_dir.path()));
        ASSERT_TRUE(WorldSaveService::finalize_indexed_envelope(save_dir.path()));
        ASSERT_TRUE(transaction.commit(nullptr));
    }
    Luminumbra::Persistence::SnapshotIndex index;
    ASSERT_TRUE(Luminumbra::Persistence::read_snapshot_index(save_dir.path(), index, nullptr));
    EXPECT_TRUE(std::none_of(index.members.begin(), index.members.end(), [](const auto& member) {
        return member.path == FieldPageState::kRelativePath;
    }));
    FieldPageState state;
    AddFieldSection(state, FieldSection::Pages);
    ASSERT_TRUE(WorldSaveService::write_file_durably(
        WorldSaveService::field_pages_path(save_dir.path()), state.serialize()));
    FieldPageState loaded;
    AddFieldSection(loaded, FieldSection::Clock);
    const auto before = loaded.serialize();
    std::vector<std::string> errors;
    EXPECT_FALSE(WorldSaveService::load_field_pages(loaded, save_dir.path(), &errors));
    EXPECT_EQ(loaded.serialize(), before);
    EXPECT_NE(JoinErrors(errors).find("inconsistent snapshot: chunks/region/field-pages.fpg"),
              std::string::npos)
        << JoinErrors(errors);
    errors.clear();
    WorldStreamingState restored;
    EXPECT_FALSE(WorldSaveService{}.load_world(restored, save_dir.path(), errors));
    EXPECT_NE(JoinErrors(errors).find("inconsistent snapshot: chunks/region/field-pages.fpg"),
              std::string::npos)
        << JoinErrors(errors);
}

enum class FieldMemberDamage {
    Deleted,
    Checksum,
    Size
};

void ExpectFieldSnapshotMismatch(FieldMemberDamage damage) {
    const TempSaveDir save_dir("field_snapshot_mismatch");
    FieldPageState state;
    AddFieldSection(state, FieldSection::Clock);
    std::vector<std::string> errors;
    {
        Luminumbra::Persistence::SaveTransaction transaction(
            save_dir.path(), 0, {}, WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(&errors)) << JoinErrors(errors);
        ASSERT_TRUE(WorldSaveService::save_field_pages(state, save_dir.path(), &errors));
        ASSERT_TRUE(WorldSaveService::finalize_indexed_envelope(save_dir.path(), &errors));
        ASSERT_TRUE(transaction.commit(&errors)) << JoinErrors(errors);
    }
    const auto path = WorldSaveService::field_pages_path(save_dir.path());
    if (damage == FieldMemberDamage::Deleted) {
        ASSERT_TRUE(std::filesystem::remove(path));
    } else {
        FieldPageState replacement = state;
        ASSERT_TRUE(replacement.set_weather_age({1, 0}, 14));
        if (damage == FieldMemberDamage::Size) {
            AddFieldSection(replacement, FieldSection::Soil);
            ASSERT_NE(replacement.serialize().size(), state.serialize().size());
        } else {
            ASSERT_EQ(replacement.serialize().size(), state.serialize().size());
        }
        ASSERT_TRUE(WorldSaveService::write_file_durably(path, replacement.serialize()));
    }
    FieldPageState loaded;
    AddFieldSection(loaded, FieldSection::Pages);
    const auto before = loaded.serialize();
    EXPECT_FALSE(WorldSaveService::load_field_pages(loaded, save_dir.path(), &errors));
    EXPECT_EQ(loaded.serialize(), before);
    EXPECT_NE(JoinErrors(errors).find("inconsistent snapshot: chunks/region/field-pages.fpg"),
              std::string::npos)
        << JoinErrors(errors);
}

std::map<std::string, std::string> FieldSaveBytes(const std::filesystem::path& root) {
    std::map<std::string, std::string> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file())
            result.emplace(Luminumbra::Filesystem::GenericUtf8PathString(entry.path()),
                           ReadFileBytes(entry.path()));
    }
    return result;
}

void ExpectEmptyFieldPreservesLegacyBytes() {
    const TempSaveDir save_dir("field_legacy");
    WorldStreamingState state;
    PopulateFixtureWorld(state);
    ASSERT_TRUE(WorldSaveService{}.save_world(state, save_dir.path()));
    const auto before = FieldSaveBytes(save_dir.path());
    ASSERT_FALSE(before.empty());
    ASSERT_FALSE(Luminumbra::Persistence::snapshot_index_exists(save_dir.path()));
    ASSERT_TRUE(WorldSaveService::save_field_pages({}, save_dir.path()));
    EXPECT_EQ(FieldSaveBytes(save_dir.path()), before);
    EXPECT_FALSE(Luminumbra::Persistence::snapshot_index_exists(save_dir.path()));
}

void ExpectLegacyFieldAuthorityRefused() {
    const TempSaveDir save_dir("field_legacy_authority");
    WorldStreamingState state;
    PopulateFixtureWorld(state);
    ASSERT_TRUE(WorldSaveService{}.save_world(state, save_dir.path()));
    ASSERT_FALSE(Luminumbra::Persistence::snapshot_index_exists(save_dir.path()));
    const auto before = FieldSaveBytes(save_dir.path());
    FieldPageState fields;
    AddFieldSection(fields, FieldSection::Clock);
    std::vector<std::string> errors;
    EXPECT_FALSE(WorldSaveService::save_field_pages(fields, save_dir.path(), &errors));
    EXPECT_NE(JoinErrors(errors).find("nonempty state requires an indexed transaction"),
              std::string::npos)
        << JoinErrors(errors);
    EXPECT_EQ(FieldSaveBytes(save_dir.path()), before);
    EXPECT_FALSE(Luminumbra::Persistence::journal_pending(save_dir.path()));
    EXPECT_FALSE(Luminumbra::Persistence::snapshot_index_exists(save_dir.path()));
    EXPECT_FALSE(std::filesystem::exists(WorldSaveService::field_pages_path(save_dir.path())));
    ASSERT_TRUE(WorldSaveService::write_file_durably(
        WorldSaveService::field_pages_path(save_dir.path()), fields.serialize()));
    const auto field_bytes = fields.serialize();
    errors.clear();
    EXPECT_FALSE(WorldSaveService::load_field_pages(fields, save_dir.path(), &errors));
    EXPECT_EQ(fields.serialize(), field_bytes);
    EXPECT_NE(JoinErrors(errors).find("field authority without index"), std::string::npos);
    errors.clear();
    WorldStreamingState restored;
    EXPECT_FALSE(WorldSaveService{}.load_world(restored, save_dir.path(), errors));
    EXPECT_NE(JoinErrors(errors).find("field authority without index"), std::string::npos)
        << JoinErrors(errors);
}
} // namespace

TEST(WorldSaveService, FieldPagesPreparedRoundTripRollbackAndEmptyRemoval) {
    ExpectFieldRoundTrip();
}

TEST(WorldSaveService, CorruptFieldPagesRefuseWithoutChangingOutput) {
    ExpectBadFieldMember(false);
}

TEST(WorldSaveService, OversizedFieldPagesRefuseWithoutChangingOutput) {
    ExpectBadFieldMember(true);
}

TEST(WorldSaveService, FieldPagesRequireLedgerAndEveryCellAndEvolutionOwner) {
    ExpectOrphanFieldSection(FieldSection::Pages, false);
    for (const auto section : {FieldSection::Pages,
                               FieldSection::Storm,
                               FieldSection::Strike,
                               FieldSection::Scent,
                               FieldSection::Clock,
                               FieldSection::Soil,
                               FieldSection::Irrigation}) {
        ExpectOrphanFieldSection(section, true);
    }
}

TEST(WorldSaveService, FieldPageDigestCompositionPreservesAbsentMember) {
    ExpectFieldDigest(false, false);
    ExpectFieldDigest(true, false);
    ExpectFieldDigest(false, true);
    ExpectFieldDigest(true, true);
}

TEST(WorldSaveService, EmptyFieldPagesPreserveLegacySaveBytes) {
    ExpectEmptyFieldPreservesLegacyBytes();
}

TEST(WorldSaveService, LegacyFieldAuthorityRequiresAnIndexedTransaction) {
    ExpectLegacyFieldAuthorityRefused();
}

TEST(WorldSaveService, StandaloneFieldLoadVerifiesCommittedPresenceSizeAndChecksum) {
    ExpectFieldSnapshotMismatch(FieldMemberDamage::Deleted);
    ExpectFieldSnapshotMismatch(FieldMemberDamage::Checksum);
    ExpectFieldSnapshotMismatch(FieldMemberDamage::Size);
}

TEST(WorldSaveService, NeverWrittenFieldMemberStaysUnlistedAndInjectedMemberIsRefused) {
    ExpectUnlistedFieldMemberRefused();
}

namespace {
void CheckJointEnergyLedgerAdmission(bool present,
                                     bool missing_age,
                                     bool legacy_phase,
                                     bool legacy_without_ages = false) {
    using namespace Luminumbra::world;
    namespace P = Luminumbra::Persistence;
    const TempSaveDir save("joint_energy_ledger");
    const WorldClock clock(31);
    ActiveRegionLedger ledger({}, clock);
    ledger.mark_edited({0, 0}, clock);
    ledger.mark_edited({2, 0}, clock);
    ASSERT_TRUE(ledger.set_energy_age({0, 0}, 7));
    ASSERT_TRUE(ledger.set_energy_age({2, 0}, 5)); // No EFS values belong to this owner.
    if (legacy_without_ages) {
        ledger.reset_energy_ages();
    }
    nlohmann::json metadata = {{"container_version", WorldSaveService::kContainerVersion}};
    clock.write_metadata(metadata);
    const std::string record =
        legacy_without_ages ? "EFS1 2 3\nP 0 0 0:60000\n" : "EFS1 2 0\nP 0 0 0:60000\n";
    {
        WorldSaveService::SaveFactsScope facts(save.path());
        P::SaveTransaction transaction(save.path(), 31, {}, &WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(WorldSaveService::save_metadata_and_active_regions(
            metadata.dump(), ledger, save.path()));
        if (present) {
            ASSERT_TRUE(transaction.protect({"aether_state.efs"}, nullptr));
            ASSERT_TRUE(
                WorldSaveService::write_member_bytes(save.path(), "aether_state.efs", record));
        }
        ASSERT_TRUE(WorldSaveService::finalize_indexed_envelope(save.path()));
        ASSERT_TRUE(transaction.commit(nullptr));
    }
    ASSERT_TRUE(WorldSaveService::validate_save(save.path()));
    ActiveRegionLedger restored;
    ASSERT_TRUE(WorldSaveService::load_active_regions(restored, save.path()));
    EXPECT_EQ(restored.energy_ages(), ledger.energy_ages());
    if (missing_age) {
        ledger.reset_energy_ages();
        ASSERT_TRUE(ledger.set_energy_age({2, 0}, 5));
        ASSERT_TRUE(WorldSaveService::write_file_durably(
            WorldSaveService::active_regions_path(save.path()), ledger.encode()));
    }
    if (legacy_phase) {
        ASSERT_TRUE(WorldSaveService::write_file_durably(save.path() / "aether_state.efs",
                                                         "EFS1 2 3\nP 0 0 0:60000\n"));
    }
    std::vector<std::string> errors;
    EXPECT_EQ(WorldSaveService::validate_save(save.path(), &errors), !missing_age && !legacy_phase);
    if (missing_age || legacy_phase) {
        EXPECT_NE(
            JoinErrors(errors).find("expected EFS1 cadence offset 0 with matching ARL energy ages"),
            std::string::npos)
            << JoinErrors(errors);
        P::MemberDigest digest;
        std::string error;
        EXPECT_FALSE(
            WorldSaveService::member_digest(save.path(), "aether_state.efs", digest, error));
        EXPECT_NE(error.find("matching ARL energy ages"), std::string::npos);
    }
}
} // namespace

TEST(WorldSaveService, RegionalEnergyAndLedgerAgesAreAdmittedTogether) {
    ASSERT_NO_FATAL_FAILURE(CheckJointEnergyLedgerAdmission(false, false, false));
    ASSERT_NO_FATAL_FAILURE(CheckJointEnergyLedgerAdmission(true, false, false));
    ASSERT_NO_FATAL_FAILURE(CheckJointEnergyLedgerAdmission(true, true, false));
    ASSERT_NO_FATAL_FAILURE(CheckJointEnergyLedgerAdmission(true, false, true));
    ASSERT_NO_FATAL_FAILURE(CheckJointEnergyLedgerAdmission(true, false, false, true));
}

namespace {
void CheckCanonicalLegacyEnergyAdmission(bool with_ledger) {
    using namespace Luminumbra::world;
    const TempSaveDir save("canonical_legacy_energy");
    const WorldClock clock(31);
    nlohmann::json metadata = {{"container_version", WorldSaveService::kContainerVersion}};
    clock.write_metadata(metadata);
    ASSERT_TRUE(
        WorldSaveService::write_file_durably(save.path() / "world_info.json", metadata.dump()));
    if (with_ledger) {
        const ActiveRegionLedger ledger({}, clock);
        ASSERT_TRUE(WorldSaveService::write_file_durably(
            WorldSaveService::active_regions_path(save.path()), ledger.encode()));
    }
    const std::pair<std::string, bool> cases[] = {
        {"EFS1 2 8\nP 0 0 0:1 0:2\n", false},
        {"", false},
        {"EFS1 2 8\nP 0 0 1:2 0:1\n", false},
        {"EFS1 2 8\nP 0 0 0:1\nP 0 0 1:2\n", false},
        {"EFS1 2 8\nP 1 0 0:1\nP 0 0 0:1\n", false},
        {"EFS1 2 8\nP 0 0\n", false},
        {"EFS1 2 8\nP 0 0 0:0\n", false},
        {"EFS1 2 8\nP 0 0 512:1\n", false},
        {"EFS1 2 8\nP 0 0 0:65536\n", false},
        {"EFS1 02 8\nP 0 0 0:1\n", false},
        {"EFS1 2 08\nP 0 0 0:1\n", false},
        {"EFS1 2 8\nP -0 0 0:1\n", false},
        {"EFS1 2 8\nP -01 0 0:1\n", false},
        {"EFS1 2 8\nP +1 0 0:1\n", false},
        {"EFS1 2 8\nP 0 0 00:1\n", false},
        {"EFS1 2 8\nP 0 0 0:01\n", false},
        {"EFS1\t2 8\nP 0 0 0:1\n", false},
        {"EFS1 2 8\nP  0 0 0:1\n", false},
        {"EFS1 2 8\nP 0 0 0:1 \n", false},
        {"EFS1 2 8\r\nP 0 0 0:1\n", false},
        {"EFS1 2 8\n\nP 0 0 0:1\n", false},
        {"EFS1 2 8\nP 0 0 0:1", false},
        {"EFS1 2 8\nP 0 0 0:1\njunk\n", false},
        {"EFS1 2 8\n", true},
        {"EFS1 2 8\nP -1 0 0:1\nP 0 0 0:2 511:65535\n", true},
        {"EFS1 2 8\nP 0 0 0:1\nP 0 -1 0:1\n", true},
        {"EFS1 2 8\nP -2147483648 -2147483648 0:1\nP 2147483647 2147483647 0:1\n", true},
        {"EFS1 2 1\nP 0 0 0:1\n", true}};
    for (const auto& [record, expected] : cases) {
        SCOPED_TRACE(record);
        ASSERT_TRUE(WorldSaveService::write_file_durably(save.path() / "aether_state.efs", record));
        std::vector<std::string> errors;
        if (with_ledger) {
            EXPECT_EQ(WorldSaveService::validate_save(save.path(), &errors), expected)
                << JoinErrors(errors);
        }
        Luminumbra::Persistence::MemberDigest digest;
        std::string error;
        EXPECT_EQ(WorldSaveService::member_digest(save.path(), "aether_state.efs", digest, error),
                  expected)
            << error;
    }
}

void CheckLegacyEnergyReaderPageLimit(std::size_t pages) {
    using namespace Luminumbra::world;
    const TempSaveDir save("legacy_energy_reader_limit");
    const WorldClock clock(31);
    const ActiveRegionLedger ledger({}, clock);
    nlohmann::json metadata = {{"container_version", WorldSaveService::kContainerVersion}};
    clock.write_metadata(metadata);
    // Write an unindexed, previously readable envelope directly: the writer has
    // a deliberately smaller page budget than the reader.
    ASSERT_TRUE(
        WorldSaveService::write_file_durably(save.path() / "world_info.json", metadata.dump()));
    ASSERT_TRUE(WorldSaveService::write_file_durably(
        WorldSaveService::active_regions_path(save.path()), ledger.encode()));
    std::string record = "EFS1 2 8\n";
    for (std::size_t page = 0; page < pages; ++page) {
        record += "P " + std::to_string(page) + " 0 0:1\n";
    }
    ASSERT_TRUE(WorldSaveService::write_file_durably(save.path() / "aether_state.efs", record));
    std::vector<std::string> errors;
    ActiveRegionLedger restored;
    // The region ledger loads on its own; the legacy energy record is admitted by validation.
    EXPECT_TRUE(WorldSaveService::load_active_regions(restored, save.path(), &errors))
        << JoinErrors(errors);
    errors.clear();
    EXPECT_EQ(WorldSaveService::validate_save(save.path(), &errors), pages <= 8192)
        << JoinErrors(errors);
    Luminumbra::Persistence::MemberDigest digest;
    std::string error;
    EXPECT_EQ(WorldSaveService::member_digest(save.path(), "aether_state.efs", digest, error),
              pages <= 8192)
        << error;
}
} // namespace

TEST(WorldSaveService, LegacyEnergyAdmissionRequiresCanonicalCellsAndBytes) {
    for (const bool with_ledger : {true, false}) {
        SCOPED_TRACE(with_ledger);
        ASSERT_NO_FATAL_FAILURE(CheckCanonicalLegacyEnergyAdmission(with_ledger));
    }
}

TEST(WorldSaveService, LegacyEnergyAdmissionRetainsTheReaderPageRange) {
    for (const std::size_t pages : {4097u, 8192u, 8193u}) {
        SCOPED_TRACE(pages);
        ASSERT_NO_FATAL_FAILURE(CheckLegacyEnergyReaderPageLimit(pages));
    }
}

namespace {
enum class PlantAdmissionCase {
    Valid,
    MissingLedger,
    Unallocated,
    Collision,
    Orphan,
    FutureStamp,
    FutureGrowth
};

Luminumbra::Ecs::EntityRegistrySnapshot
WorldPlantSnapshot(bool persistent, PlantAdmissionCase admission = PlantAdmissionCase::Valid) {
    namespace C = Luminumbra::Components;
    entt::registry plants;
    const auto plant = plants.create();
    plants.emplace<C::PlantTag>(plant);
    plants.emplace<C::TransformComponent>(plant).position = Vec3{1, 2, -1};
    plants.emplace<C::PlantGenomeComponent>(plant);
    plants.emplace<C::PlantGrowthComponent>(plant).last_tick =
        admission == PlantAdmissionCase::FutureStamp ||
                admission == PlantAdmissionCase::FutureGrowth
            ? 32
            : 31;
    plants.emplace<C::PlantRegionComponent>(
        plant,
        C::PlantRegionComponent{
            1, 0, -1, admission == PlantAdmissionCase::FutureStamp ? 32u : 31u});
    plants.emplace<C::CombustibleComponent>(plant);
    plants.emplace<C::MortalComponent>(plant);
    plants.emplace<C::GrazeableComponent>(plant);
    plants.emplace<C::WaterSourceComponent>(plant);
    plants.emplace<C::IgnitionSourceComponent>(plant);
    plants.emplace<C::PollinationTag>(plant);
    return luminumbra::foliage::BuildPlantEntitySnapshot(plants, persistent);
}

void WriteIndexedPlants(const std::filesystem::path& directory,
                        const Luminumbra::Ecs::EntityRegistrySnapshot& plants,
                        PlantAdmissionCase admission,
                        std::uint64_t tick = 31,
                        bool idle_regions = false) {
    namespace P = Luminumbra::Persistence;
    using namespace Luminumbra::world;
    const WorldClock clock(tick);
    ActiveRegionLedger ledger({}, clock);
    if (admission != PlantAdmissionCase::Orphan) {
        if (idle_regions) {
            ledger.activate_owned({0, -1}, clock);
            ledger.activate_owned({1, 0}, clock);
        } else {
            ledger.mark_edited({0, -1}, clock);
        }
    }
    if (admission != PlantAdmissionCase::Unallocated) {
        for (std::size_t i = 0; i < std::max(std::size_t{1}, plants.entities.size()); ++i) {
            std::uint64_t id = 0;
            ASSERT_TRUE(ledger.allocate_persistent_id(id));
            ASSERT_EQ(id, i + 1u);
        }
    }
    nlohmann::json metadata = {{"container_version", WorldSaveService::kContainerVersion}};
    clock.write_metadata(metadata);
    WorldSaveService::SaveFactsScope facts(directory);
    P::SaveTransaction transaction(directory, tick, {}, &WorldSaveService::member_digest);
    std::vector<std::string> errors;
    ASSERT_TRUE(transaction.begin(&errors)) << JoinErrors(errors);
    if (admission == PlantAdmissionCase::MissingLedger) {
        // A repeat publication keeps the committed metadata member untouched.
        if (!std::filesystem::exists(directory / "world_info.json")) {
            ASSERT_TRUE(transaction.protect({"world_info.json"}, &errors)) << JoinErrors(errors);
            ASSERT_TRUE(WorldSaveService::write_member_bytes(
                directory, "world_info.json", metadata.dump(), &errors))
                << JoinErrors(errors);
        }
    } else {
        ASSERT_TRUE(
            WorldSaveService::save_metadata_and_active_regions(metadata.dump(), ledger, directory));
    }
    if (admission == PlantAdmissionCase::Collision) {
        GroundObjectStore ground;
        ASSERT_TRUE(ground.insert({.id = 1, .type = 1, .z_mm = -1000}));
        ASSERT_TRUE(WorldSaveService::save_ground_objects(ground, directory, &errors))
            << JoinErrors(errors);
    }
    ASSERT_TRUE(WorldSaveService::save_plant_entities(plants, directory, &errors))
        << JoinErrors(errors);
    ASSERT_TRUE(WorldSaveService::finalize_indexed_envelope(directory, &errors))
        << JoinErrors(errors);
    ASSERT_TRUE(transaction.commit(&errors)) << JoinErrors(errors);
}

void CheckWorldPlantAdmission(PlantAdmissionCase admission, bool persistent = true) {
    const TempSaveDir save("plant_identity");
    auto original = WorldPlantSnapshot(persistent, admission);
    ASSERT_NO_FATAL_FAILURE(WriteIndexedPlants(save.path(), original, admission));
    const auto path = WorldSaveService::plant_entities_path(save.path());
    const auto before = ReadFileBytes(path);
    std::string legacy_bytes;
    ASSERT_TRUE(WorldSaveService::prepare_plant_entities(original, legacy_bytes));
    EXPECT_EQ(before, legacy_bytes);
    if (admission != PlantAdmissionCase::FutureStamp &&
        admission != PlantAdmissionCase::FutureGrowth) {
        EXPECT_EQ(before,
                  Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(WorldPlantSnapshot(false)));
    }
    EXPECT_EQ(std::filesystem::exists(WorldSaveService::plant_identities_path(save.path())),
              persistent);
    Luminumbra::Persistence::SnapshotIndex index;
    ASSERT_TRUE(Luminumbra::Persistence::read_snapshot_index(save.path(), index, nullptr));
    EXPECT_EQ(std::any_of(index.members.begin(),
                          index.members.end(),
                          [](const auto& member) {
                              return member.path == "chunks/region/plant-identities.json" &&
                                     member.present;
                          }),
              persistent);
    const bool valid = !persistent || admission == PlantAdmissionCase::Valid;
    std::vector<std::string> errors;
    WorldStreamingState chunks;
    EXPECT_EQ(WorldSaveService{}.load_world(chunks, save.path(), errors), valid)
        << JoinErrors(errors);
    if (!valid) {
        EXPECT_TRUE(chunks.empty());
        EXPECT_NE(JoinErrors(errors).find("Corrupt plant authority:"), std::string::npos)
            << JoinErrors(errors);
    } else {
        Luminumbra::Ecs::EntityRegistrySnapshot restored;
        ASSERT_TRUE(WorldSaveService::load_plant_entities(restored, save.path(), &errors))
            << JoinErrors(errors);
        entt::registry registry;
        static_cast<void>(registry.create());
        ASSERT_TRUE(luminumbra::foliage::ApplyPlantEntitySnapshot(registry, restored));
        const auto projected = luminumbra::foliage::BuildPlantEntitySnapshot(registry, persistent);
        if (persistent) {
            EXPECT_EQ(Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(projected),
                      Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(original));
        }
        // Repeat publication preserves the exact bytes, including legacy worlds without identity.
        ASSERT_NO_FATAL_FAILURE(WriteIndexedPlants(save.path(), restored, admission));
        EXPECT_EQ(ReadFileBytes(path), before);
        EXPECT_TRUE(WorldSaveService::validate_save(save.path(), &errors)) << JoinErrors(errors);
    }
    EXPECT_EQ(ReadFileBytes(path), before);
}
} // namespace

TEST(WorldSaveService, PersistentPlantsReopenWithStableIdsOwnersAndEnabledComponents) {
    CheckWorldPlantAdmission(PlantAdmissionCase::Valid);
}
TEST(WorldSaveService, PersistentPlantsRequireAllocatedUniqueIdsAndLedgerOwners) {
    CheckWorldPlantAdmission(PlantAdmissionCase::MissingLedger);
    CheckWorldPlantAdmission(PlantAdmissionCase::Unallocated);
    CheckWorldPlantAdmission(PlantAdmissionCase::Collision);
    CheckWorldPlantAdmission(PlantAdmissionCase::Orphan);
    CheckWorldPlantAdmission(PlantAdmissionCase::FutureStamp);
}
TEST(WorldSaveService, PersistentPlantGrowthCannotBeAheadOfLedgerTick) {
    CheckWorldPlantAdmission(PlantAdmissionCase::FutureGrowth);
}
TEST(WorldSaveService, LegacyPlantsRemainByteIdenticalWithoutIdentityOrLedger) {
    CheckWorldPlantAdmission(PlantAdmissionCase::MissingLedger, false);
}

namespace {
class PersistentPlantSessionTest : public ::testing::Test {
protected:
    void SetUp() override {
        const std::filesystem::path source(LUMINUMBRA_SOURCE_ROOT);
        const auto presets = root.path() / "worlds/atlas/presets";
        std::filesystem::create_directories(presets);
        std::filesystem::copy_file(source / "worlds/atlas/presets/default.json",
                                   presets / "default.json");
        Luminumbra::TestSupport::CopyWorldgenTestData(source, root.path());
        jobs.startup(1);
        session.SetRootPath(root.path().string() +
                            static_cast<char>(std::filesystem::path::preferred_separator));
        session.SetJobSystem(&jobs);
        session.SetActiveRegionsEnabled(true);
        session.SetAetherStateEnabled(false);
        ASSERT_TRUE(session.CreateWorld("Plant authority", "1337", "default"));
        ASSERT_NO_FATAL_FAILURE(WriteIndexedPlants(
            root.path() / "valid", WorldPlantSnapshot(true), PlantAdmissionCase::Valid));
        ASSERT_TRUE(session.LoadWorldStateFrom(root.path() / "valid"))
            << session.GetWorldOpenError();
    }
    TempSaveDir root{"plant_session"};
    Luminumbra::JobSystem jobs;
    Luminumbra::world::GameSession session;
};
} // namespace

TEST_F(PersistentPlantSessionTest, LegacyAndMissingRostersReplacePersistentPlants) {
    namespace C = Luminumbra::Components;
    for (const bool empty : {false, true}) {
        SCOPED_TRACE(empty);
        ASSERT_TRUE(session.LoadWorldStateFrom(root.path() / "valid"));
        auto& registry = session.GetRegistry();
        const auto old = *registry.view<C::PlantTag>().begin();
        const auto candidate = root.path() / (empty ? "empty" : "legacy");
        ASSERT_NO_FATAL_FAILURE(WriteIndexedPlants(candidate,
                                                   empty ? Luminumbra::Ecs::EntityRegistrySnapshot{}
                                                         : WorldPlantSnapshot(false),
                                                   PlantAdmissionCase::Valid));
        EXPECT_EQ(std::filesystem::exists(WorldSaveService::plant_entities_path(candidate)),
                  !empty);
        ASSERT_TRUE(session.LoadWorldStateFrom(candidate)) << session.GetWorldOpenError();
        EXPECT_FALSE(registry.valid(old));
        EXPECT_EQ(registry.view<C::PlantTag>().size(), empty ? 0u : 1u);
        EXPECT_FALSE(luminumbra::foliage::HasPersistentPlantIdentity(registry));
        EXPECT_EQ(session.GetActiveRegionLedger().next_persistent_id(), 2u);
        const auto output = root.path() / "replacement-save";
        ASSERT_TRUE(session.SaveWorldStateTo(output));
        EXPECT_TRUE(WorldSaveService::validate_save(output));
    }
}

TEST_F(PersistentPlantSessionTest, LegacyRostersStillAppendAndMissingRosterPreservesLegacyPlants) {
    namespace C = Luminumbra::Components;
    for (const bool enabled : {false, true}) {
        SCOPED_TRACE(enabled);
        session.SetActiveRegionsEnabled(enabled);
        ASSERT_TRUE(session.CreateWorld("Legacy plants", "1337", "default"));
        auto& registry = session.GetRegistry();
        ASSERT_TRUE(
            luminumbra::foliage::ApplyPlantEntitySnapshot(registry, WorldPlantSnapshot(false)));
        const auto old = *registry.view<C::PlantTag>().begin();
        for (const bool empty : {false, true}) {
            const auto candidate = root.path() / (empty ? "empty" : "legacy");
            ASSERT_TRUE(WorldSaveService::save_plant_entities(
                empty ? Luminumbra::Ecs::EntityRegistrySnapshot{} : WorldPlantSnapshot(false),
                candidate));
            ASSERT_TRUE(session.LoadWorldStateFrom(candidate)) << session.GetWorldOpenError();
            EXPECT_TRUE(registry.valid(old));
            EXPECT_EQ(registry.view<C::PlantTag>().size(), 2u);
            EXPECT_FALSE(luminumbra::foliage::HasPersistentPlantIdentity(registry));
        }
    }
}

TEST_F(PersistentPlantSessionTest, IdlePlantOnlyOwnerSurvivesSchedulingAndRoundTrips) {
    namespace C = Luminumbra::Components;
    using namespace Luminumbra::world;
    const RegionKey owner{0, -1}, unowned{1, 0};
    const auto input = root.path() / "idle";
    ASSERT_NO_FATAL_FAILURE(WriteIndexedPlants(input,
                                               WorldPlantSnapshot(true),
                                               PlantAdmissionCase::Valid,
                                               ActiveRegionLedger::kIdleEvictionTicks,
                                               true));
    ASSERT_TRUE(session.LoadWorldStateFrom(input)) << session.GetWorldOpenError();
    for (const auto key : {owner, unowned}) {
        const auto& record = session.GetActiveRegionLedger().records().at(key);
        ASSERT_FALSE(record.edited || record.pinned || record.populated);
        ASSERT_EQ(record.water_cursor, 0u);
        ASSERT_EQ(record.last_proximity, 0u);
        ASSERT_FALSE(session.GetWorldSystem()->has_region_members(key.x, key.z));
        ASSERT_FALSE(session.GetGroundObjects().owns_region(key));
        ASSERT_FALSE(session.GetFieldPages().owns_region(key));
    }
    ASSERT_TRUE(session.GetActiveRegionLedger().energy_ages().empty());
    ASSERT_TRUE(session.GetActiveRegionLedger().water_debts().empty());
    ASSERT_TRUE(session.GetActiveRegionLedger().completed_work().empty());
    session.SetReplicatedSimulationAnchors({});
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_TRUE(session.GetActiveRegionLedger().records().contains(owner));
    EXPECT_FALSE(session.GetActiveRegionLedger().records().contains(unowned));
    const auto plants = session.ComputePlantSubHash();
    const auto output = root.path() / "idle-roundtrip";
    ASSERT_TRUE(session.SaveWorldStateTo(output));
    ASSERT_TRUE(session.LoadWorldStateFrom(output)) << session.GetWorldOpenError();
    EXPECT_EQ(session.ComputePlantSubHash(), plants);
    EXPECT_TRUE(session.GetActiveRegionLedger().records().contains(owner));
    EXPECT_EQ(session.GetRegistry().view<C::PlantTag>().size(), 1u);
    EXPECT_TRUE(luminumbra::foliage::HasPersistentPlantIdentity(session.GetRegistry()));
}

TEST_F(PersistentPlantSessionTest, PlantSubHashOrdersIdentifiedPlantsByPersistentIdNotHandle) {
    namespace C = Luminumbra::Components;
    auto& registry = session.GetRegistry();
    const auto stale_view = registry.view<C::PlantTag>();
    const std::vector<entt::entity> stale(stale_view.begin(), stale_view.end());
    registry.destroy(stale.begin(), stale.end());
    const auto spawn = [&registry](std::uint64_t id, std::uint16_t species) {
        const auto plant = registry.create();
        registry.emplace<C::PlantTag>(plant);
        C::PlantGrowthComponent growth;
        growth.species_id = species;
        registry.emplace<C::PlantGrowthComponent>(plant, growth);
        C::PlantRegionComponent owner;
        owner.persistent_id = id;
        registry.emplace<C::PlantRegionComponent>(plant, owner);
    };
    spawn(5, 1);
    spawn(9, 2);
    const std::string forward = session.ComputePlantSubHash();
    ASSERT_FALSE(forward.empty());
    const auto replaced_view = registry.view<C::PlantTag>();
    const std::vector<entt::entity> replaced(replaced_view.begin(), replaced_view.end());
    registry.destroy(replaced.begin(), replaced.end());
    for (int i = 0; i < 3; ++i) {
        registry.destroy(registry.create());
    }
    spawn(9, 2);
    spawn(5, 1);
    EXPECT_EQ(session.ComputePlantSubHash(), forward);
}

TEST_F(PersistentPlantSessionTest, WorldPlantAuthorityRefusalPreservesResidentChunksAndPlants) {
    auto chunk = std::make_shared<Chunk>(IVec3{0, 0, 0});
    chunk->set_state(ChunkState::Idle);
    ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
    const auto chunks = session.GetWorldSystem()->snapshot_streamed_chunks();
    ASSERT_FALSE(chunks.empty());
    const auto plants = session.ComputePlantSubHash();
    const auto ledger = session.GetActiveRegionLedger().canonical_bytes();
    for (const auto admission : {PlantAdmissionCase::Unallocated,
                                 PlantAdmissionCase::Collision,
                                 PlantAdmissionCase::Orphan,
                                 PlantAdmissionCase::FutureStamp,
                                 PlantAdmissionCase::FutureGrowth}) {
        const TempSaveDir candidate("refused_plant");
        ASSERT_NO_FATAL_FAILURE(
            WriteIndexedPlants(candidate.path(), WorldPlantSnapshot(true, admission), admission));
        EXPECT_FALSE(session.LoadWorldStateFrom(candidate.path()));
        EXPECT_EQ(session.GetWorldOpenError().find("Corrupt plant authority:"), 0u);
        EXPECT_EQ(session.GetWorldSystem()->snapshot_streamed_chunks(), chunks);
        EXPECT_EQ(session.ComputePlantSubHash(), plants);
        EXPECT_EQ(session.GetActiveRegionLedger().canonical_bytes(), ledger);
        ASSERT_TRUE(session.SaveWorldStateTo(root.path() / "still-usable"));
    }
}

TEST_F(PersistentPlantSessionTest,
       InvalidPlantAuthorityRefusesSaveBeforeWritingAndValidRosterReloads) {
    namespace C = Luminumbra::Components;
    const auto plants = session.GetRegistry().view<C::PlantRegionComponent>();
    ASSERT_NE(plants.begin(), plants.end());
    auto& owner = plants.get<C::PlantRegionComponent>(*plants.begin());
    const auto original = owner;
    owner.persistent_id = session.GetActiveRegionLedger().next_persistent_id();
    const auto unallocated = root.path() / "unallocated";
    EXPECT_FALSE(session.SaveWorldStateTo(unallocated));
    EXPECT_FALSE(std::filesystem::exists(unallocated));
    const auto collision = session.SpawnGroundObject(1, 1000, 2000, -1000, 0, 0);
    ASSERT_NE(collision, 0u);
    owner.persistent_id = collision;
    const auto duplicate = root.path() / "collision";
    EXPECT_FALSE(session.SaveWorldStateTo(duplicate));
    EXPECT_FALSE(std::filesystem::exists(duplicate));
    owner = original;
    auto& growth = session.GetRegistry().get<C::PlantGrowthComponent>(*plants.begin());
    growth.last_tick = 32;
    for (const std::uint64_t stamp : {31u, 32u}) {
        owner.region_stamp = stamp;
        const auto future = root.path() / "future";
        EXPECT_FALSE(session.SaveWorldStateTo(future));
        EXPECT_FALSE(std::filesystem::exists(future));
    }
    growth.last_tick = 31;
    owner = original;
    auto& position = session.GetRegistry().get<C::TransformComponent>(*plants.begin()).position;
    position.x = 1025;
    owner.owner_x = 2;
    const auto orphan = root.path() / "orphan";
    EXPECT_FALSE(session.SaveWorldStateTo(orphan));
    EXPECT_FALSE(std::filesystem::exists(orphan));
    position.x = 1;
    owner = original;
    const auto before = session.ComputePlantSubHash();
    const auto valid = root.path() / "roundtrip";
    ASSERT_TRUE(session.SaveWorldStateTo(valid));
    ASSERT_TRUE(session.LoadWorldStateFrom(valid)) << session.GetWorldOpenError();
    EXPECT_EQ(session.ComputePlantSubHash(), before);
}

namespace {
// Malformed fixtures still publish a complete index; admission, rather than fixture writing,
// must reject the deliberately corrupt authority.
bool RawPlantFixtureDigest(const std::filesystem::path& directory,
                           std::string_view relative,
                           Luminumbra::Persistence::MemberDigest& digest,
                           std::string& error) {
    bool exists = false;
    std::vector<std::string> errors;
    const bool read = Luminumbra::Persistence::hash_file(
        directory / relative, digest.bytes, digest.fnv64, exists, &errors);
    digest.present = exists;
    if (!read && exists) {
        error = JoinErrors(errors);
        return false;
    }
    return true;
}

void PublishPlantFixture(const std::filesystem::path& directory,
                         const std::string& payload,
                         const std::string& identity) {
    namespace P = Luminumbra::Persistence;
    std::vector<std::string> errors;
    P::SaveTransaction transaction(directory, 31, {}, &RawPlantFixtureDigest);
    ASSERT_TRUE(transaction.begin(&errors)) << JoinErrors(errors);
    for (const auto& member : {std::pair{"chunks/region/plant-entities.json", &payload},
                               std::pair{"chunks/region/plant-identities.json", &identity}}) {
        ASSERT_TRUE(transaction.protect({member.first}, &errors)) << JoinErrors(errors);
        if (member.second->empty()) {
            ASSERT_TRUE(WorldSaveService::remove_member(directory, member.first, &errors))
                << JoinErrors(errors);
        } else {
            ASSERT_TRUE(WorldSaveService::write_member_bytes(
                directory, member.first, *member.second, &errors))
                << JoinErrors(errors);
        }
    }
    ASSERT_TRUE(transaction.commit(&errors)) << JoinErrors(errors);
}

void CheckPlantMapRefusal(const std::string& kind) {
    namespace E = Luminumbra::Ecs;
    const TempSaveDir save("plant_map_refusal");
    const auto original = WorldPlantSnapshot(true);
    ASSERT_NO_FATAL_FAILURE(WriteIndexedPlants(save.path(), original, PlantAdmissionCase::Valid));
    auto payload = ReadFileBytes(WorldSaveService::plant_entities_path(save.path()));
    auto map =
        nlohmann::json::parse(ReadFileBytes(WorldSaveService::plant_identities_path(save.path())));
    if (kind == "digest") {
        payload += " ";
    }
    if (kind == "same_length_digest") {
        std::swap(payload[0], payload[1]);
    }
    if (kind == "unknown_metadata") {
        map["unknown"] = 0u;
    }
    if (kind == "unknown_component") {
        map["plants"][0]["components"][0]["type"] = "Future";
    }
    if (kind == "count") {
        map["plants"] = nlohmann::json::array();
    }
    if (kind == "version") {
        map["version"] = 2u;
    }
    if (kind == "orphan") {
        payload.clear();
    }
    if (kind == "empty") {
        payload = E::SerializeEntityRegistrySnapshotJson({});
        map["plants"] = nlohmann::json::array();
    }
    if (kind == "duplicate" || kind == "duplicate_payload") {
        auto parsed = nlohmann::json::parse(payload);
        parsed["entities"].push_back(parsed["entities"][0]);
        parsed["entities"][1]["entity_id"] = 2u;
        parsed["entity_count"] = 2u;
        parsed["component_count"] = 2u * parsed["component_count"].get<unsigned>();
        payload = E::StableEntitySnapshotDump(parsed);
        map["plants"].push_back(map["plants"][0]);
        if (kind == "duplicate_payload") {
            parsed["entities"][1]["entity_id"] = parsed["entities"][0]["entity_id"];
            payload = E::StableEntitySnapshotDump(parsed);
            map["plants"][1]["persistent_id"] = 2u;
        }
    }
    if (kind == "unallocated") {
        map["plants"][0]["persistent_id"] = 2u;
    }
    if (kind == "negative") {
        map["plants"][0]["persistent_id"] = -1;
    }
    if (kind == "missing") {
        map["plants"][0].erase("owner_x");
    }
    if (kind == "component") {
        map["plants"][0]["components"][0]["data"] = nullptr;
    }
    if (kind != "digest" && kind != "same_length_digest" && kind != "orphan") {
        map["payload_bytes"] = payload.size();
        map["payload_fnv64"] =
            Luminumbra::Persistence::fnv1a64_bytes(payload.data(), payload.size());
    }
    const auto identity = kind == "partial" ? std::string("{") : map.dump();
    ASSERT_NO_FATAL_FAILURE(PublishPlantFixture(save.path(), payload, identity));
    auto loaded = original;
    std::vector<std::string> errors;
    if (kind != "unallocated") {
        EXPECT_FALSE(WorldSaveService::load_plant_entities(loaded, save.path(), &errors));
        EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(loaded),
                  E::SerializeEntityRegistrySnapshotJson(original));
    }
    WorldStreamingState chunks;
    errors.clear();
    EXPECT_FALSE(WorldSaveService{}.load_world(chunks, save.path(), errors));
    EXPECT_TRUE(chunks.empty());
    EXPECT_NE(JoinErrors(errors).find("Corrupt plant authority:"), std::string::npos)
        << JoinErrors(errors);
    EXPECT_FALSE(WorldSaveService::validate_save(save.path()));
}
} // namespace

TEST(WorldSaveService, MalformedPersistentPlantReadLeavesTheOutputUnchanged) {
    for (const auto* kind : {"digest",
                             "version",
                             "orphan",
                             "empty",
                             "duplicate",
                             "unallocated",
                             "negative",
                             "missing",
                             "component",
                             "partial",
                             "same_length_digest",
                             "unknown_metadata",
                             "unknown_component",
                             "count",
                             "duplicate_payload"}) {
        SCOPED_TRACE(kind);
        ASSERT_NO_FATAL_FAILURE(CheckPlantMapRefusal(kind));
    }
}

TEST(WorldSaveService, PersistentGameplayPlantsSurviveGrowthAndHandleHistory) {
    namespace F = luminumbra::foliage;
    namespace E = Luminumbra::Ecs;
    namespace C = Luminumbra::Components;
    const TempSaveDir save("plant_growth_identity");
    auto original = WorldPlantSnapshot(true);
    auto second = original.entities.front();
    second.legacy_entity_id = 1u;
    original.entities.front().entity_id = 2u;
    original.entities.push_back(second);
    ASSERT_NO_FATAL_FAILURE(WriteIndexedPlants(save.path(), original, PlantAdmissionCase::Valid));
    E::EntityRegistrySnapshot loaded;
    ASSERT_TRUE(WorldSaveService::load_plant_entities(loaded, save.path()));
    std::string continuation;
    for (const unsigned history : {0u, 5u}) {
        entt::registry registry;
        for (unsigned i = 0; i < history; ++i) {
            const auto dummy = registry.create();
            registry.destroy(dummy);
        }
        ASSERT_TRUE(F::ApplyPlantEntitySnapshot(registry, loaded));
        ASSERT_TRUE(F::HasPersistentPlantIdentity(registry));
        for (const auto entity : registry.view<C::PlantGrowthComponent>()) {
            ++registry.get<C::PlantGrowthComponent>(entity).last_tick;
        }
        E::EntityRegistrySnapshot gameplay;
        std::string error;
        ASSERT_TRUE(F::BuildPlantEntitySnapshot(registry, gameplay, error)) << error;
        if (history == 0) {
            continuation = E::SerializeEntityRegistrySnapshotJson(gameplay);
        } else {
            EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(gameplay), continuation);
        }
        ASSERT_NO_FATAL_FAILURE(
            WriteIndexedPlants(save.path(), gameplay, PlantAdmissionCase::Valid, 32));
        ASSERT_TRUE(std::filesystem::exists(WorldSaveService::plant_identities_path(save.path())));
        E::EntityRegistrySnapshot reopened;
        ASSERT_TRUE(WorldSaveService::load_plant_entities(reopened, save.path()));
        EXPECT_EQ(E::SerializeEntityRegistrySnapshotJson(reopened), continuation);
        EXPECT_TRUE(WorldSaveService::validate_save(save.path()));
    }
}

TEST(WorldSaveService, PlantMapRetiresForLegacyAndEmptyRosters) {
    for (const bool empty : {false, true}) {
        const TempSaveDir save("plant_map_retirement");
        ASSERT_NO_FATAL_FAILURE(
            WriteIndexedPlants(save.path(), WorldPlantSnapshot(true), PlantAdmissionCase::Valid));
        const auto replacement =
            empty ? Luminumbra::Ecs::EntityRegistrySnapshot{} : WorldPlantSnapshot(false);
        ASSERT_NO_FATAL_FAILURE(
            WriteIndexedPlants(save.path(), replacement, PlantAdmissionCase::Valid));
        EXPECT_FALSE(std::filesystem::exists(WorldSaveService::plant_identities_path(save.path())));
        EXPECT_TRUE(WorldSaveService::validate_save(save.path()));
    }
}

TEST(WorldSaveService, PlantIdentityMapMustBeListedInTheCommitIndex) {
    namespace P = Luminumbra::Persistence;
    const TempSaveDir save("plant_map_index");
    ASSERT_NO_FATAL_FAILURE(
        WriteIndexedPlants(save.path(), WorldPlantSnapshot(true), PlantAdmissionCase::Valid));
    P::SnapshotIndex index;
    ASSERT_TRUE(P::read_snapshot_index(save.path(), index, nullptr));
    std::erase_if(index.members, [](const auto& member) {
        return member.path == "chunks/region/plant-identities.json";
    });
    // Deliberately corrupt the commit index after publishing the indexed fixture.
    ASSERT_TRUE(WorldSaveService::write_file_durably(P::snapshot_index_path(save.path()),
                                                     P::encode_snapshot_index(index)));
    std::vector<std::string> errors;
    EXPECT_FALSE(WorldSaveService::validate_save(save.path(), &errors));
    EXPECT_NE(JoinErrors(errors).find("inconsistent snapshot:"), std::string::npos)
        << JoinErrors(errors);
}

TEST(WorldSaveService, AbandonedPlantPublicationRestoresBothMembers) {
    namespace P = Luminumbra::Persistence;
    const TempSaveDir save("plant_map_rollback");
    auto plants = WorldPlantSnapshot(true);
    ASSERT_NO_FATAL_FAILURE(WriteIndexedPlants(save.path(), plants, PlantAdmissionCase::Valid));
    const auto payload_path = WorldSaveService::plant_entities_path(save.path());
    const auto map_path = WorldSaveService::plant_identities_path(save.path());
    const auto payload = ReadFileBytes(payload_path);
    const auto identity = ReadFileBytes(map_path);
    const auto index = ReadFileBytes(P::snapshot_index_path(save.path()));
    for (auto& component : plants.entities.front().components) {
        if (component.type == "PlantGrowth") {
            component.data["last_tick"] = 32u;
        }
        if (component.type == "Mortal") {
            component.data["age_ticks"] = 1u;
        }
    }
    {
        P::SaveTransaction transaction(save.path(), 31, {}, &WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(WorldSaveService::save_plant_entities(plants, save.path()));
        EXPECT_NE(ReadFileBytes(payload_path), payload);
        EXPECT_NE(ReadFileBytes(map_path), identity);
    }
    EXPECT_EQ(ReadFileBytes(payload_path), payload);
    EXPECT_EQ(ReadFileBytes(map_path), identity);
    EXPECT_EQ(ReadFileBytes(P::snapshot_index_path(save.path())), index);
    EXPECT_TRUE(WorldSaveService::validate_save(save.path()));
}

namespace {
std::string SavedWaterFixture(const IVec3& coords) {
    WorldStreamingState state;
    auto chunk = state.get_or_create_chunk(coords);
    chunk->set_state(ChunkState::Idle);
    chunk->has_water_sim.store(true);
    chunk->current_water_resolution.store(4);
    chunk->water_depth_mm.assign(16, 1000);
    chunk->water_bed_mm.assign(16, -100);
    chunk->water_edge_flux.assign(32, -10);
    chunk->water_level_data.assign(16, 0.9f);
    chunk->water_flow_data.assign(16, Vec2(0.1f, -0.2f));
    chunk->water_sim_terrain_height.assign(16, -0.1f);
    chunk->mark_water_data_dirty();
    return Luminumbra::Persistence::SerializeWorldStreamingStateSnapshotJson(state, true);
}

void WriteWaterRecord(const std::filesystem::path& root,
                      const IVec3& coords,
                      const std::string& payload,
                      Luminumbra::u8 flags = 0) {
    int rx = 0, rz = 0;
    WorldSaveService::region_coords_for_chunk(coords, rx, rz);
    ASSERT_TRUE(WorldSaveService::upsert_container_records(
        WorldSaveService::region_file_path(root, rx, rz),
        {{Chunk::calculate_id(coords), 0, flags, payload}}));
}

// Build an internally matching index without admitting the deliberately malformed payload.
void IndexWaterFixture(const std::filesystem::path& root) {
    namespace P = Luminumbra::Persistence;
    P::SnapshotIndex index;
    index.generation = 1;
    index.txn_id = 1;
    for (const int rx : {0, 1}) {
        const auto raw = ReadFileBytes(WorldSaveService::region_file_path(root, rx, 0));
        ASSERT_GE(raw.size(), 26u);
        ASSERT_EQ(static_cast<unsigned char>(raw[6]), 1u);
        ASSERT_EQ(static_cast<unsigned char>(raw[7]), 0u);
        // One lod-0 record: omit the file header and the record's lod byte.
        const auto stream = raw.substr(8, 8) + raw.substr(17);
        index.members.push_back({.path = "chunks/region/r." + std::to_string(rx) + ".0.lmr",
                                 .bytes = static_cast<std::uint64_t>(stream.size()),
                                 .fnv64 = P::fnv1a64_bytes(stream.data(), stream.size()),
                                 .generation = 1});
    }
    ASSERT_TRUE(WorldSaveService::write_file_durably(P::snapshot_index_path(root),
                                                     P::encode_snapshot_index(index)));
}
} // namespace

TEST(WorldSaveService, IndexedWaterRefusalCoversEveryReaderAndCachedLegacyAdmission) {
    const IVec3 good{0, 0, 0}, bad{32, 0, 0};
    for (int variant = 0; variant < 8; ++variant) {
        SCOPED_TRACE(variant);
        const TempSaveDir save("strict_water");
        auto payload = nlohmann::json::parse(SavedWaterFixture(bad));
        auto& water = payload["chunks"][0];
        if (variant == 0) {
            water["water_depth_mm"][0] = -1;
        }
        if (variant == 1) {
            water["water_bed_mm"] = nlohmann::json::array();
        }
        if (variant == 2) {
            water.erase("water_edge_flux");
        }
        if (variant == 3) {
            water["water_flow_data"][0]["x"] = 1e100;
        }
        if (variant == 4) {
            water["water_state"]["water_mesh_dirty_ticks"] = 16;
        }
        if (variant == 5) {
            water["water_state"]["current_water_resolution"] = 5;
        }
        if (variant == 6) {
            water["water_state"]["ticks_below_threshold"] = -1;
        }
        if (variant == 7) {
            water["current_water_resolution"] = 8;
        }
        ASSERT_NO_FATAL_FAILURE(WriteWaterRecord(save.path(), good, SavedWaterFixture(good)));
        ASSERT_NO_FATAL_FAILURE(WriteWaterRecord(
            save.path(), bad, payload.dump(), static_cast<Luminumbra::u8>(variant % 2)));
        const auto region = WorldSaveService::region_file_path(save.path(), 1, 0);
        const auto legacy_bytes = ReadFileBytes(region);
        const WorldSaveService::WorldReadScope reads;
        std::vector<std::string> errors;
        ASSERT_TRUE(WorldSaveService::validate_save(save.path(), &errors));
        WorldStreamingState loaded;
        ASSERT_TRUE(WorldSaveService{}.load_world(loaded, save.path(), errors));
        EXPECT_EQ(loaded.size(), 2u);
        EXPECT_EQ(ReadFileBytes(region), legacy_bytes);
        ASSERT_NO_FATAL_FAILURE(IndexWaterFixture(save.path()));
        errors.clear();
        EXPECT_FALSE(WorldSaveService::validate_save(save.path(), &errors));
        EXPECT_FALSE(WorldSaveService{}.load_world(loaded, save.path(), errors));
        EXPECT_EQ(loaded.size(), 0u);
        EXPECT_NE(JoinErrors(errors).find("Saved water"), std::string::npos);
        EXPECT_EQ(WorldSaveService::read_chunk(save.path(), bad).status,
                  WorldSaveService::ChunkReadStatus::Error);
        std::vector<std::shared_ptr<Chunk>> chunks;
        EXPECT_FALSE(WorldSaveService::read_region_chunks(region, chunks));
        EXPECT_TRUE(chunks.empty());
        EXPECT_FALSE(
            WorldSaveService::read_authoritative_region_chunks(region, 32, 32, 0, 0, chunks));
        EXPECT_TRUE(chunks.empty());
        EXPECT_EQ(ReadFileBytes(region), legacy_bytes);
    }
}

TEST(WorldSaveService, FirstIndexedSaveValidatesEveryRetainedLegacyWaterRecordAndRollsBack) {
    namespace P = Luminumbra::Persistence;
    for (const int retained_region : {0, 1}) {
        const TempSaveDir save("water_upgrade");
        const IVec3 retained{32 * retained_region, 0, 0};
        auto payload = nlohmann::json::parse(SavedWaterFixture(retained));
        payload["chunks"][0]["water_depth_mm"][0] = 2147483648ULL;
        ASSERT_NO_FATAL_FAILURE(WriteWaterRecord(save.path(), retained, payload.dump()));
        ASSERT_TRUE(WorldSaveService::validate_save(save.path()));
        const auto region = WorldSaveService::region_file_path(save.path(), retained_region, 0);
        const auto before = ReadFileBytes(region);
        std::vector<std::string> errors;
        P::SaveTransaction transaction(save.path(), 0, {}, WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(&errors)) << JoinErrors(errors);
        WorldStreamingState replacement;
        replacement.get_or_create_chunk({1, 0, 0})->set_state(ChunkState::Idle);
        ASSERT_TRUE(WorldSaveService{}.save_world(replacement, save.path(), &errors));
        ASSERT_TRUE(WorldSaveService::finalize_indexed_envelope(save.path(), &errors));
        EXPECT_FALSE(transaction.commit(&errors));
        EXPECT_NE(JoinErrors(errors).find("Saved water"), std::string::npos);
        EXPECT_EQ(ReadFileBytes(region), before);
        EXPECT_FALSE(P::snapshot_index_exists(save.path()));
        EXPECT_FALSE(P::journal_pending(save.path()));
    }
}

TEST(WorldSaveService, IndexedWaterRoundTripsBytesAndRefusesMissingRecords) {
    namespace P = Luminumbra::Persistence;
    const TempSaveDir save("water_roundtrip");
    for (const IVec3 coords : {IVec3{0, 0, 0}, IVec3{32, 0, 0}}) {
        ASSERT_NO_FATAL_FAILURE(WriteWaterRecord(save.path(), coords, SavedWaterFixture(coords)));
    }
    std::vector<std::string> errors;
    {
        P::SaveTransaction transaction(save.path(), 0, {}, WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(&errors));
        ASSERT_TRUE(WorldSaveService::finalize_indexed_envelope(save.path(), &errors));
        ASSERT_TRUE(transaction.commit(&errors)) << JoinErrors(errors);
    }
    WorldStreamingState loaded;
    ASSERT_TRUE(WorldSaveService{}.load_world(loaded, save.path(), errors));
    const auto bytes = P::SerializeWorldStreamingStateSnapshotJson(loaded, true);
    ASSERT_TRUE(WorldSaveService{}.save_world(loaded, save.path(), &errors));
    ASSERT_TRUE(WorldSaveService{}.load_world(loaded, save.path(), errors));
    EXPECT_EQ(P::SerializeWorldStreamingStateSnapshotJson(loaded, true), bytes);
    const auto region = WorldSaveService::region_file_path(save.path(), 1, 0);
    const auto region_bytes = ReadFileBytes(region);
    ASSERT_TRUE(WorldSaveService{}.save_world(loaded, save.path(), &errors));
    EXPECT_EQ(ReadFileBytes(region), region_bytes);
    ASSERT_TRUE(WorldSaveService::remove_file_and_staging(region));
    EXPECT_FALSE(WorldSaveService{}.load_world(loaded, save.path(), errors));
    EXPECT_EQ(loaded.size(), 0u);
    EXPECT_EQ(WorldSaveService::read_chunk(save.path(), {32, 0, 0}).status,
              WorldSaveService::ChunkReadStatus::Error);
}

TEST(WorldSaveService, IndexedWaterRejectsOwnersDuplicateRecordsAndIdentityMismatch) {
    const IVec3 good{0, 0, 0}, bad{32, 0, 0};
    for (int variant = 0; variant < 7; ++variant) {
        SCOPED_TRACE(variant);
        const TempSaveDir save("water_identity");
        auto payload = nlohmann::json::parse(SavedWaterFixture(bad));
        if (variant == 0) {
            payload["chunks"][0]["water_authority"] = "edited";
        }
        if (variant == 1) {
            payload["chunks"][0]["authority"]["water_owned"] = false;
        }
        if (variant == 2) {
            payload["chunks"].push_back(payload["chunks"][0]);
            payload["chunk_count"] = 2;
        }
        if (variant == 3) {
            payload["chunk_count"] = 2;
        }
        if (variant == 4) {
            payload["chunks"][0]["coords"]["x"] = 33;
        }
        if (variant == 5) {
            payload["chunks"][0]["coords"]["x"] = 1048608;
        }
        if (variant == 6) {
            payload["chunks"][0]["authority"]["terrain_edited"] = true;
            payload["chunks"][0]["water_authority"] = "edited";
            payload["chunks"][0]["water_state"]["terrain_authoritative"] = true;
        }
        ASSERT_NO_FATAL_FAILURE(WriteWaterRecord(save.path(), good, SavedWaterFixture(good)));
        ASSERT_NO_FATAL_FAILURE(WriteWaterRecord(save.path(), bad, payload.dump()));
        ASSERT_NO_FATAL_FAILURE(IndexWaterFixture(save.path()));
        WorldStreamingState loaded;
        std::vector<std::string> errors;
        EXPECT_FALSE(WorldSaveService{}.load_world(loaded, save.path(), errors));
        EXPECT_EQ(loaded.size(), 0u);
        EXPECT_FALSE(errors.empty());
        EXPECT_EQ(WorldSaveService::read_chunk(save.path(), bad).status,
                  WorldSaveService::ChunkReadStatus::Error);
    }
}

namespace {
using AuthorityLimits = WorldSaveService::AuthorityReadLimits;
using AuthorityReader = WorldSaveService::AuthorityWindowReader;
using AuthorityMembership = WorldSaveService::AuthorityMembership;

AuthorityLimits WindowLimits() {
    return {.members = 64,
            .records = 128,
            .headers = 256,
            .probes = 256,
            .compressed_member_bytes = 4u * 1024u * 1024u,
            .compressed_total_bytes = 16u * 1024u * 1024u,
            .decoded_member_bytes = 4u * 1024u * 1024u,
            .decoded_total_bytes = 16u * 1024u * 1024u,
            .control_bytes = 128u * 1024u};
}

std::shared_ptr<Chunk>
AddWindowChunk(WorldStreamingState& state, const IVec3& coords, bool water = false) {
    auto chunk = state.get_or_create_chunk(coords);
    chunk->set_state(ChunkState::Ready);
    constexpr std::size_t lattice = static_cast<std::size_t>(Luminumbra::CHUNK_SIZE_X + 1) *
                                    (Luminumbra::CHUNK_SIZE_Y + 1) * (Luminumbra::CHUNK_SIZE_Z + 1);
    chunk->sdf_data.resize(lattice);
    for (std::size_t cell = 0; cell < lattice; ++cell)
        chunk->sdf_data[cell] = static_cast<float>(cell % 101u) - 49.5f;
    chunk->mark_voxel_data_dirty();
    if (water) {
        chunk->current_water_resolution.store(8, std::memory_order_release);
        chunk->water_bed_mm.assign(64, 1000);
        chunk->water_depth_mm.assign(64, 2000);
        chunk->set_water_authority();
    }
    return chunk;
}

void WriteWindowWorld(const std::filesystem::path& root,
                      const WorldStreamingState& state,
                      bool indexed = true) {
    namespace P = Luminumbra::Persistence;
    std::vector<std::string> errors;
    if (!indexed) {
        ASSERT_TRUE(WorldSaveService{}.save_world(state, root, &errors)) << JoinErrors(errors);
        return;
    }
    P::SaveTransaction transaction(root, 0, {}, WorldSaveService::member_digest);
    ASSERT_TRUE(transaction.begin(&errors)) << JoinErrors(errors);
    ASSERT_TRUE(WorldSaveService{}.save_world(state, root, &errors)) << JoinErrors(errors);
    ASSERT_TRUE(WorldSaveService::finalize_indexed_envelope(root, &errors)) << JoinErrors(errors);
    ASSERT_TRUE(transaction.commit(&errors)) << JoinErrors(errors);
}

void ReplaceWindowBytes(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    ASSERT_TRUE(output.good());
}

void ExpectWindowUnavailable(const WorldSaveService::AuthorityWindowResult& result) {
    EXPECT_EQ(result.membership, AuthorityMembership::Unavailable);
    EXPECT_TRUE(result.chunks.empty());
    EXPECT_FALSE(result.error.empty());
}
} // namespace

TEST(WorldSaveService, AuthorityWindowOwnsDiskOnlyTerrainWaterAndBoundaryContributors) {
    const TempSaveDir save("authority_window_owned");
    WorldStreamingState state;
    auto terrain = AddWindowChunk(state, {0, 0, 0});
    auto boundary = AddWindowChunk(state, {-1, 2, -1}, true);
    AddWindowChunk(state, {20, 0, 20});
    ASSERT_NO_FATAL_FAILURE(WriteWindowWorld(save.path(), state));
    AuthorityReader reader;
    std::string error;
    ASSERT_TRUE(reader.admit(save.path(), WindowLimits(), error)) << error;
    const auto result = reader.read({}, WindowLimits());
    ASSERT_EQ(result.membership, AuthorityMembership::Present) << result.error;
    ASSERT_EQ(result.chunks.size(), 2u);
    EXPECT_GT(result.generation, 0u);
    EXPECT_GT(result.source_identity, 0u);
    for (const auto& chunk : result.chunks) {
        EXPECT_NE(chunk.get(), terrain.get());
        EXPECT_NE(chunk.get(), boundary.get());
        EXPECT_EQ(chunk->sdf_data, terrain->sdf_data);
        if (chunk->get_coords().x == -1) {
            EXPECT_TRUE(chunk->has_water_authority());
            EXPECT_EQ(chunk->water_depth_mm, boundary->water_depth_mm);
        }
    }
    terrain->sdf_data.front() = 999.0f;
    EXPECT_NE(result.chunks.front()->sdf_data.front(), 999.0f);
    EXPECT_TRUE(std::is_sorted(
        result.chunks.begin(), result.chunks.end(), [](const auto& lhs, const auto& rhs) {
            return lhs->get_id() < rhs->get_id();
        }));
    EXPECT_TRUE(WorldSaveService::remove_file_and_staging(
        WorldSaveService::region_file_path(save.path(), 0, 0)));
    EXPECT_FALSE(result.chunks.front()->sdf_data.empty());
}

TEST(WorldSaveService, AuthorityWindowCompleteEmptyInventoryProvesAbsence) {
    const TempSaveDir save("authority_window_empty");
    ASSERT_NO_FATAL_FAILURE(WriteWindowWorld(save.path(), {}));
    AuthorityReader reader;
    std::string error;
    ASSERT_TRUE(reader.admit(save.path(), WindowLimits(), error)) << error;
    const auto result = reader.read({}, WindowLimits());
    EXPECT_EQ(result.membership, AuthorityMembership::ProvenAbsent) << result.error;
    EXPECT_TRUE(result.error.empty());
    EXPECT_TRUE(result.chunks.empty());
    ASSERT_EQ(result.members.size(), 4u);
    for (const auto& member : result.members) {
        EXPECT_EQ(member.membership, AuthorityMembership::ProvenAbsent);
    }
}

TEST(WorldSaveService, AuthorityWindowRefusesMissingExpectedMemberAndRecord) {
    for (const bool remove_member : {false, true}) {
        const TempSaveDir save("authority_window_missing");
        WorldStreamingState state;
        AddWindowChunk(state, {0, 0, 0});
        ASSERT_NO_FATAL_FAILURE(WriteWindowWorld(save.path(), state));
        AuthorityReader reader;
        std::string error;
        ASSERT_TRUE(reader.admit(save.path(), WindowLimits(), error)) << error;
        const auto path = WorldSaveService::region_file_path(save.path(), 0, 0);
        if (remove_member) {
            ASSERT_TRUE(std::filesystem::remove(path));
        } else {
            ASSERT_NO_FATAL_FAILURE(ReplaceWindowBytes(path, std::string("LMR1\2\0\0\0", 8)));
        }
        ExpectWindowUnavailable(reader.read({}, WindowLimits()));
    }
}

TEST(WorldSaveService, AuthorityWindowRefusesChangedCommitAndInvalidMemberGenerations) {
    namespace P = Luminumbra::Persistence;
    const TempSaveDir save("authority_window_generation");
    WorldStreamingState state;
    auto chunk = AddWindowChunk(state, {0, 0, 0});
    ASSERT_NO_FATAL_FAILURE(WriteWindowWorld(save.path(), state));
    AuthorityReader reader;
    std::string error;
    ASSERT_TRUE(reader.admit(save.path(), WindowLimits(), error)) << error;
    chunk->sdf_data.front() = 321.0f;
    chunk->mark_voxel_data_dirty();
    ASSERT_NO_FATAL_FAILURE(WriteWindowWorld(save.path(), state));
    ExpectWindowUnavailable(reader.read({}, WindowLimits()));
    P::SnapshotIndex index;
    ASSERT_TRUE(P::read_snapshot_index(save.path(), index, nullptr));
    ASSERT_FALSE(index.members.empty());
    const auto original = index.members.front().generation;
    for (const auto generation : {std::uint64_t{0}, index.generation + 1}) {
        index.members.front().generation = generation;
        ASSERT_NO_FATAL_FAILURE(ReplaceWindowBytes(P::snapshot_index_path(save.path()),
                                                   P::encode_snapshot_index(index)));
        EXPECT_FALSE(reader.admit(save.path(), WindowLimits(), error));
        EXPECT_FALSE(error.empty());
    }
    index.members.front().generation = original;
    ASSERT_NO_FATAL_FAILURE(
        ReplaceWindowBytes(P::snapshot_index_path(save.path()), P::encode_snapshot_index(index)));
    ASSERT_TRUE(reader.admit(save.path(), WindowLimits(), error)) << error;
    EXPECT_EQ(reader.read({}, WindowLimits()).membership, AuthorityMembership::Present);
}

TEST(WorldSaveService, AuthorityWindowRefusesMalformedPartialAndUnsupportedContainers) {
    for (const int damage : {0, 1, 2, 3, 4}) {
        const TempSaveDir save("authority_window_malformed");
        WorldStreamingState state;
        AddWindowChunk(state, {0, 0, 0});
        ASSERT_NO_FATAL_FAILURE(WriteWindowWorld(save.path(), state));
        const auto path = WorldSaveService::region_file_path(save.path(), 0, 0);
        auto bytes = ReadFileBytes(path);
        ASSERT_GT(bytes.size(), 26u);
        if (damage == 0)
            bytes.resize(4);
        else if (damage == 1)
            bytes.pop_back();
        else if (damage == 2)
            bytes[4] = 3;
        else if (damage == 3)
            bytes[8 + 8] = 3;
        else
            bytes.back() ^= 0x7f;
        ASSERT_NO_FATAL_FAILURE(ReplaceWindowBytes(path, bytes));
        AuthorityReader reader;
        std::string error;
        ASSERT_TRUE(reader.admit(save.path(), WindowLimits(), error)) << error;
        ExpectWindowUnavailable(reader.read({}, WindowLimits()));
    }
}

TEST(WorldSaveService, AuthorityWindowRefusesPartialLatticeAndWaterBeforeReturningAuthority) {
    for (const int damage : {0, 1, 2, 3, 4}) {
        const TempSaveDir save("authority_window_partial_truth");
        WorldStreamingState state;
        auto chunk = AddWindowChunk(state, {0, 0, 0}, damage >= 2);
        if (damage == 0)
            chunk->sdf_data.pop_back();
        else if (damage == 1)
            chunk->sdf_data.clear();
        else if (damage == 2)
            chunk->water_depth_mm.pop_back();
        else if (damage == 3)
            chunk->current_water_resolution.store(3, std::memory_order_release);
        else
            chunk->water_depth_mm.front() = -1;
        // Raw container writing deliberately bypasses the ordinary writer's semantic gate.
        const WorldSaveService::ContainerRecord record{
            chunk->get_id(),
            0,
            1,
            Luminumbra::Persistence::SerializeChunkRecordPayloadJson(*chunk)};
        ASSERT_TRUE(WorldSaveService::upsert_container_records(
            WorldSaveService::region_file_path(save.path(), 0, 0), {record}));
        AuthorityReader reader;
        std::string error;
        EXPECT_FALSE(reader.admit(save.path(), WindowLimits(), error));
        EXPECT_FALSE(error.empty());
        ExpectWindowUnavailable(reader.read({}, WindowLimits()));
    }
}

TEST(WorldSaveService, AuthorityWindowEveryFiniteCapRefusesWithoutDecoding) {
    const TempSaveDir save("authority_window_caps");
    WorldStreamingState state;
    AddWindowChunk(state, {0, 0, 0});
    AddWindowChunk(state, {20, 0, 20});
    ASSERT_NO_FATAL_FAILURE(WriteWindowWorld(save.path(), state));
    AuthorityReader reader;
    std::string error;
    ASSERT_TRUE(reader.admit(save.path(), WindowLimits(), error)) << error;
    const std::array fields{&AuthorityLimits::members,
                            &AuthorityLimits::records,
                            &AuthorityLimits::headers,
                            &AuthorityLimits::probes,
                            &AuthorityLimits::compressed_member_bytes,
                            &AuthorityLimits::compressed_total_bytes,
                            &AuthorityLimits::decoded_member_bytes,
                            &AuthorityLimits::decoded_total_bytes,
                            &AuthorityLimits::control_bytes};
    for (const auto field : fields) {
        for (const std::uint64_t cap : {std::uint64_t{0}, std::uint64_t{1}}) {
            auto limits = WindowLimits();
            limits.*field = cap;
            if (cap == 0) {
                EXPECT_FALSE(reader.admit(save.path(), limits, error));
            }
            const auto decodes = WorldSaveService::chunk_record_decode_count_for_testing();
            ExpectWindowUnavailable(reader.read({}, limits));
            EXPECT_EQ(WorldSaveService::chunk_record_decode_count_for_testing(), decodes);
        }
    }
    EXPECT_EQ(reader.read({}, WindowLimits()).membership, AuthorityMembership::Present);
}

TEST(WorldSaveService, AuthorityWindowLegacyAdmissionPinsGenerationZeroWithoutCreatingIndex) {
    namespace P = Luminumbra::Persistence;
    const TempSaveDir save("authority_window_legacy");
    WorldStreamingState state;
    AddWindowChunk(state, {-32, 0, 0}, true);
    ASSERT_NO_FATAL_FAILURE(WriteWindowWorld(save.path(), state, false));
    AuthorityReader reader;
    std::string error;
    ASSERT_TRUE(reader.admit(save.path(), WindowLimits(), error)) << error;
    const auto result = reader.read({-32, -32, 0, 0}, WindowLimits());
    ASSERT_EQ(result.membership, AuthorityMembership::Present) << result.error;
    EXPECT_EQ(result.generation, 0u);
    EXPECT_GT(result.source_identity, 0u);
    ASSERT_EQ(result.chunks.size(), 1u);
    EXPECT_TRUE(result.chunks.front()->has_water_authority());
    EXPECT_FALSE(std::filesystem::exists(P::snapshot_index_path(save.path())));
    AuthorityReader second;
    ASSERT_TRUE(second.admit(save.path(), WindowLimits(), error)) << error;
    EXPECT_NE(second.read({-32, -32, 0, 0}, WindowLimits()).source_identity,
              result.source_identity);
    const auto path = WorldSaveService::region_file_path(save.path(), -1, 0);
    const auto bytes = ReadFileBytes(path);
    std::filesystem::rename(path, path.parent_path() / "retained-original");
    ASSERT_NO_FATAL_FAILURE(ReplaceWindowBytes(path, bytes));
    ExpectWindowUnavailable(reader.read({-32, -32, 0, 0}, WindowLimits()));
}

TEST(WorldSaveService, AuthorityWindowRefusesNoncanonicalRegionMembersAndPreservesAdmission) {
    namespace P = Luminumbra::Persistence;
    const TempSaveDir original_save("authority_window_canonical");
    WorldStreamingState state;
    const auto chunk = AddWindowChunk(state, {0, 0, 0}, true);
    ASSERT_NO_FATAL_FAILURE(WriteWindowWorld(original_save.path(), state));
    AuthorityReader reader;
    std::string error;
    ASSERT_TRUE(reader.admit(original_save.path(), WindowLimits(), error)) << error;
    const auto original = reader.read({}, WindowLimits());
    ASSERT_EQ(original.membership, AuthorityMembership::Present) << original.error;
    ASSERT_EQ(original.chunks.size(), 1u);

    for (const bool indexed : {false, true}) {
        for (const char* name : {"r.00.0.lmr", "r.0.00.lmr", "r.-0.0.lmr"}) {
            for (const bool canonical_sibling : {false, true}) {
                SCOPED_TRACE(::testing::Message()
                             << indexed << ":" << name << ":" << canonical_sibling);
                const TempSaveDir save("authority_window_noncanonical");
                ASSERT_NO_FATAL_FAILURE(WriteWindowWorld(save.path(), state, indexed));
                const auto canonical = WorldSaveService::region_file_path(save.path(), 0, 0);
                const auto alias = canonical.parent_path() / name;
                ASSERT_TRUE(std::filesystem::copy_file(canonical, alias));
                if (!canonical_sibling) {
                    ASSERT_TRUE(std::filesystem::remove(canonical));
                }
                if (indexed) {
                    P::SnapshotIndex index;
                    ASSERT_TRUE(P::read_snapshot_index(save.path(), index, nullptr));
                    const auto member = std::find_if(
                        index.members.begin(), index.members.end(), [](const auto& value) {
                            return value.path == "chunks/region/r.0.0.lmr";
                        });
                    ASSERT_NE(member, index.members.end());
                    auto alias_member = *member;
                    alias_member.path = std::string("chunks/region/") + name;
                    ASSERT_TRUE(P::member_path_allowed(alias_member.path));
                    if (canonical_sibling)
                        index.members.push_back(alias_member);
                    else
                        *member = alias_member;
                    std::sort(index.members.begin(),
                              index.members.end(),
                              [](const auto& lhs, const auto& rhs) { return lhs.path < rhs.path; });
                    ASSERT_NO_FATAL_FAILURE(ReplaceWindowBytes(P::snapshot_index_path(save.path()),
                                                               P::encode_snapshot_index(index)));
                }
                EXPECT_FALSE(reader.admit(save.path(), WindowLimits(), error));
                EXPECT_EQ(error, "unrecognised region file name");
                const auto retained = reader.read({}, WindowLimits());
                EXPECT_EQ(retained.membership, original.membership) << retained.error;
                EXPECT_EQ(retained.generation, original.generation);
                EXPECT_EQ(retained.source_identity, original.source_identity);
                ASSERT_EQ(retained.members.size(), original.members.size());
                for (std::size_t i = 0; i < retained.members.size(); ++i) {
                    EXPECT_EQ(retained.members[i].region_x, original.members[i].region_x);
                    EXPECT_EQ(retained.members[i].region_z, original.members[i].region_z);
                    EXPECT_EQ(retained.members[i].generation, original.members[i].generation);
                    EXPECT_EQ(retained.members[i].membership, original.members[i].membership);
                }
                ASSERT_EQ(retained.chunks.size(), 1u);
                EXPECT_EQ(retained.chunks.front()->sdf_data, chunk->sdf_data);
                EXPECT_EQ(retained.chunks.front()->water_depth_mm, chunk->water_depth_mm);
                EXPECT_EQ(original.chunks.front()->sdf_data, chunk->sdf_data);
                EXPECT_EQ(original.chunks.front()->water_depth_mm, chunk->water_depth_mm);
                AuthorityReader unadmitted;
                EXPECT_FALSE(unadmitted.admit(save.path(), WindowLimits(), error));
                ExpectWindowUnavailable(unadmitted.read({}, WindowLimits()));
            }
        }
    }
}

TEST(WorldSaveService, AuthorityWindowRefusesNoncanonicalIndexStatesAndClasses) {
    namespace P = Luminumbra::Persistence;
    const TempSaveDir save("authority_window_noncanonical_index");
    std::filesystem::create_directories(WorldSaveService::region_directory(save.path()));
    for (const char* name : {"r.00.0.lmr", "r.0.00.lmr", "r.2147483648.0.lmr"}) {
        for (const bool present : {false, true}) {
            for (const auto cls : {P::MemberClass::Authoritative, P::MemberClass::Derived}) {
                P::SnapshotIndex index;
                index.generation = 1;
                const auto relative = std::string("chunks/region/") + name;
                ASSERT_TRUE(P::member_path_allowed(relative));
                index.members.push_back({relative, cls, present, 0, 0, 1});
                const auto bytes = P::encode_snapshot_index(index);
                P::SnapshotIndex decoded;
                std::string error;
                ASSERT_TRUE(P::decode_snapshot_index(bytes, decoded, error)) << error;
                ASSERT_NO_FATAL_FAILURE(
                    ReplaceWindowBytes(P::snapshot_index_path(save.path()), bytes));
                AuthorityReader reader;
                EXPECT_FALSE(reader.admit(save.path(), WindowLimits(), error)) << relative;
                EXPECT_EQ(error, "unrecognised region file name") << relative;
                ExpectWindowUnavailable(reader.read({}, WindowLimits()));
            }
        }
    }
}

TEST(WorldSaveService, AuthorityWindowRefusesMalformedLegacyRegionNamesBeforeReadingPayloads) {
    const TempSaveDir save("authority_window_noncanonical_legacy");
    const auto region_dir = WorldSaveService::region_directory(save.path());
    std::filesystem::create_directories(region_dir);
    for (const char* name : {"r.00.0.lmr", "r.+0.0.lmr", "r.0.lmr", "unknown.lmr"}) {
        for (const int kind : {0, 1, 2}) {
            const auto path = region_dir / name;
            if (kind == 2) {
                std::filesystem::create_directory(path);
            } else {
                ASSERT_NO_FATAL_FAILURE(ReplaceWindowBytes(path, kind == 0 ? "" : "LMRG"));
            }
            AuthorityReader reader;
            std::string error;
            EXPECT_FALSE(reader.admit(save.path(), WindowLimits(), error)) << name;
            EXPECT_EQ(error, "unrecognised region file name") << name;
            ExpectWindowUnavailable(reader.read({}, WindowLimits()));
            ASSERT_TRUE(std::filesystem::remove(path));
        }
    }
}

namespace {
void CheckRejectedWindowArtifact(const WorldStreamingState& state,
                                 bool indexed,
                                 bool populated,
                                 const char* name,
                                 int kind,
                                 AuthorityReader& reader,
                                 const WorldSaveService::AuthorityWindowResult& original) {
    std::string error;
    const TempSaveDir save("authority_window_rejected_artifact");
    if (indexed || populated) {
        ASSERT_NO_FATAL_FAILURE(WriteWindowWorld(save.path(), state, indexed));
    }
    AuthorityReader admitted;
    ASSERT_TRUE(admitted.admit(save.path(), WindowLimits(), error)) << error;
    const auto before = admitted.read({}, WindowLimits());
    const auto path = save.path() / name;
    std::filesystem::create_directories(path.parent_path());
    if (kind == 2) {
        ASSERT_TRUE(std::filesystem::create_directory(path));
    } else {
        ASSERT_NO_FATAL_FAILURE(ReplaceWindowBytes(path, kind == 0 ? "" : "unsupported snapshot"));
    }
    WorldStreamingState rejected;
    std::vector<std::string> errors;
    EXPECT_FALSE(WorldSaveService{}.load_world(rejected, save.path(), errors));
    EXPECT_FALSE(errors.empty());
    EXPECT_FALSE(reader.admit(save.path(), WindowLimits(), error));
    EXPECT_FALSE(error.empty());
    const auto retained = reader.read({}, WindowLimits());
    EXPECT_EQ(retained.membership, original.membership) << retained.error;
    EXPECT_EQ(retained.source_identity, original.source_identity);
    EXPECT_EQ(retained.generation, original.generation);
    AuthorityReader unadmitted;
    EXPECT_FALSE(unadmitted.admit(save.path(), WindowLimits(), error));
    ExpectWindowUnavailable(unadmitted.read({}, WindowLimits()));
    if (std::string_view(name).starts_with("chunks/world-state.json")) {
        ASSERT_EQ(errors.size(), 1u);
        EXPECT_EQ(errors.front(), WorldSaveService::kObsoleteWorldMessage);
        ExpectWindowUnavailable(admitted.read({}, WindowLimits()));
    }
    ASSERT_TRUE(std::filesystem::remove(path));
    const auto restored = admitted.read({}, WindowLimits());
    EXPECT_EQ(restored.membership, before.membership) << restored.error;
    EXPECT_EQ(restored.source_identity, before.source_identity);
    ASSERT_TRUE(unadmitted.admit(save.path(), WindowLimits(), error)) << error;
}

#if !defined(_WIN32)
void CheckWindowObsoleteLinks() {
    for (const char* name : {"world-state.json", "world-state.json.bak"}) {
        for (const bool loop : {false, true}) {
            const TempSaveDir save("authority_window_obsolete_link");
            AuthorityReader reader;
            std::string error;
            ASSERT_TRUE(reader.admit(save.path(), WindowLimits(), error)) << error;
            const auto path = save.path() / "chunks" / name;
            std::filesystem::create_directory(path.parent_path());
            std::filesystem::create_symlink(loop ? name : "missing-snapshot", path);
            EXPECT_FALSE(reader.admit(save.path(), WindowLimits(), error));
            ExpectWindowUnavailable(reader.read({}, WindowLimits()));
        }
    }
}
#endif

void CheckWindowInvalidInventoryParents() {
    for (const char* parent : {"chunks", "chunks/region"}) {
        const TempSaveDir save("authority_window_uninspectable_inventory");
        AuthorityReader reader;
        std::string error;
        ASSERT_TRUE(reader.admit(save.path(), WindowLimits(), error)) << error;
        const auto path = save.path() / parent;
        std::filesystem::create_directories(path.parent_path());
        ASSERT_NO_FATAL_FAILURE(ReplaceWindowBytes(path, "not a directory"));
        EXPECT_FALSE(reader.admit(save.path(), WindowLimits(), error));
        EXPECT_FALSE(error.empty());
        ExpectWindowUnavailable(reader.read({}, WindowLimits()));
        ASSERT_TRUE(std::filesystem::remove(path));
        ASSERT_TRUE(reader.admit(save.path(), WindowLimits(), error)) << error;
    }
}

void CheckWindowInventoryProbeBudget() {
    const TempSaveDir empty("authority_window_inventory_probe_budget");
    AuthorityReader reader;
    std::string error;
    ASSERT_TRUE(reader.admit(empty.path(), WindowLimits(), error)) << error;
    const auto original = reader.read({}, WindowLimits());
    // Sweep every probe boundary through admission and both read-time envelope checks.
    for (std::uint64_t probes = 1; probes <= 18; ++probes) {
        auto limits = WindowLimits();
        limits.probes = probes;
        const bool admitted = reader.admit(empty.path(), limits, error);
        EXPECT_EQ(admitted, probes >= 15) << probes << ":" << error;
        const auto result = reader.read({}, limits);
        if (probes < 18) {
            ExpectWindowUnavailable(result);
        } else {
            EXPECT_EQ(result.membership, AuthorityMembership::ProvenAbsent) << result.error;
        }
        if (!admitted) {
            EXPECT_EQ(reader.read({}, WindowLimits()).source_identity, original.source_identity);
        }
    }
}
} // namespace

TEST(WorldSaveService, AuthorityWindowRefusesLoaderRejectedArtifactsAndPreservesAdmission) {
    for (const bool indexed : {false, true}) {
        for (const bool populated : {false, true}) {
            const TempSaveDir original_save("authority_window_original_inventory");
            WorldStreamingState state;
            if (populated) {
                AddWindowChunk(state, {0, 0, 0});
            }
            if (indexed || populated) {
                ASSERT_NO_FATAL_FAILURE(WriteWindowWorld(original_save.path(), state, indexed));
            }
            AuthorityReader reader;
            std::string error;
            ASSERT_TRUE(reader.admit(original_save.path(), WindowLimits(), error)) << error;
            const auto original = reader.read({}, WindowLimits());
            ASSERT_NE(original.membership, AuthorityMembership::Unavailable) << original.error;
            for (const char* name : {"chunks/world-state.json",
                                     "chunks/world-state.json.bak",
                                     "chunks/unknown.bin",
                                     "chunks/region/notes.txt",
                                     "chunks/region/r.00.0.txt",
                                     "chunks/region/r.00.0.lmr.tmp"}) {
                for (const int kind : {0, 1, 2}) {
                    SCOPED_TRACE(::testing::Message()
                                 << indexed << ":" << populated << ":" << name << ":" << kind);
                    ASSERT_NO_FATAL_FAILURE(CheckRejectedWindowArtifact(
                        state, indexed, populated, name, kind, reader, original));
                }
            }
        }
    }
#if !defined(_WIN32)
    ASSERT_NO_FATAL_FAILURE(CheckWindowObsoleteLinks());
#endif
    ASSERT_NO_FATAL_FAILURE(CheckWindowInvalidInventoryParents());
    ASSERT_NO_FATAL_FAILURE(CheckWindowInventoryProbeBudget());
}

TEST(WorldSaveService, AuthorityWindowUnknownIndexStatusRefusesAndPreservesAdmission) {
    namespace P = Luminumbra::Persistence;
    const TempSaveDir save("authority_window_unknown_index");
    ASSERT_NO_FATAL_FAILURE(WriteWindowWorld(save.path(), {}, false));
    AuthorityReader reader;
    std::string error;
    ASSERT_TRUE(reader.admit(save.path(), WindowLimits(), error)) << error;
    const auto original = reader.read({}, WindowLimits());
    ASSERT_EQ(original.membership, AuthorityMembership::ProvenAbsent);
    std::filesystem::create_directory(P::snapshot_index_path(save.path()));
    EXPECT_FALSE(reader.admit(save.path(), WindowLimits(), error));
    ExpectWindowUnavailable(reader.read({}, WindowLimits()));
    ASSERT_TRUE(std::filesystem::remove(P::snapshot_index_path(save.path())));
    const auto restored = reader.read({}, WindowLimits());
    EXPECT_EQ(restored.membership, AuthorityMembership::ProvenAbsent) << restored.error;
    EXPECT_EQ(restored.source_identity, original.source_identity);
}

namespace {
class WindowReplacement {
public:
    WindowReplacement() {
        attempted() = false;
        replaced() = false;
        Luminumbra::Persistence::detail::set_file_read_observer_for_testing(&Observe);
    }
    ~WindowReplacement() {
        Luminumbra::Persistence::detail::set_file_read_observer_for_testing(nullptr);
    }
    static bool& attempted() {
        static bool value = false;
        return value;
    }
    static bool& replaced() {
        static bool value = false;
        return value;
    }

private:
    static void Observe(const std::filesystem::path& path, bool opening) {
        if (!opening || attempted() || path.extension() != ".lmr")
            return;
        attempted() = true;
        std::error_code error;
        std::filesystem::rename(path, path.parent_path() / "original.lmr", error);
        if (!error) {
            std::ofstream output(path, std::ios::binary);
            output << "replacement";
            output.close();
            replaced() = output.good();
        }
    }
};
} // namespace

TEST(WorldSaveService, AuthorityWindowUsesLockedAdmittedHandleAndClosesBeforeReplacement) {
    const TempSaveDir save("authority_window_handle");
    WorldStreamingState state;
    AddWindowChunk(state, {0, 0, 0});
    ASSERT_NO_FATAL_FAILURE(WriteWindowWorld(save.path(), state));
    AuthorityReader reader;
    std::string error;
    ASSERT_TRUE(reader.admit(save.path(), WindowLimits(), error)) << error;
    {
        const WindowReplacement replacement;
        const auto result = reader.read({}, WindowLimits());
        EXPECT_TRUE(WindowReplacement::attempted());
#if defined(_WIN32)
        EXPECT_FALSE(WindowReplacement::replaced());
        EXPECT_EQ(result.membership, AuthorityMembership::Present) << result.error;
#else
        EXPECT_TRUE(WindowReplacement::replaced());
        ExpectWindowUnavailable(result);
#endif
    }
    EXPECT_TRUE(WorldSaveService::remove_file_and_staging(
        WorldSaveService::region_file_path(save.path(), 0, 0)));
}

TEST(WorldSaveService, AuthorityWindowRejectsInvalidBoundsAndIgnoresDerivedAuthority) {
    const TempSaveDir save("authority_window_derived");
    const WorldSaveService::ContainerRecord far{42, 1, 1, "derived cache"};
    ASSERT_TRUE(WorldSaveService::upsert_container_records(
        WorldSaveService::region_file_path(save.path(), 0, 0), {far}));
    AuthorityReader reader;
    std::string error;
    ASSERT_TRUE(reader.admit(save.path(), WindowLimits(), error)) << error;
    EXPECT_EQ(reader.read({}, WindowLimits()).membership, AuthorityMembership::ProvenAbsent);
    for (const auto window :
         {WorldSaveService::AuthorityWindow{2, 1, 0, 0},
          WorldSaveService::AuthorityWindow{0, 0, 2, 1},
          WorldSaveService::AuthorityWindow{std::numeric_limits<int>::min(), 0, 0, 0},
          WorldSaveService::AuthorityWindow{0, std::numeric_limits<int>::max(), 0, 0},
          WorldSaveService::AuthorityWindow{-1000000, 1000000, -1000000, 1000000}}) {
        ExpectWindowUnavailable(reader.read(window, WindowLimits()));
    }
}

TEST(WorldSaveService, AuthorityWindowContainerBudgetsApplyAfterControlAdmission) {
    namespace P = Luminumbra::Persistence;
    const TempSaveDir save("authority_window_payload_caps");
    WorldStreamingState state;
    const auto first = AddWindowChunk(state, {16, 0, 16});
    const auto second = AddWindowChunk(state, {20, 0, 20});
    ASSERT_NO_FATAL_FAILURE(WriteWindowWorld(save.path(), state));
    const auto file_bytes =
        std::filesystem::file_size(WorldSaveService::region_file_path(save.path(), 0, 0));
    const auto control_bytes =
        std::filesystem::file_size(P::snapshot_index_path(save.path())) +
        std::filesystem::file_size(WorldSaveService::world_manifest_path(save.path()));
    const auto raw_bytes = P::SerializeChunkRecordPayloadJson(*first).size() +
                           P::SerializeChunkRecordPayloadJson(*second).size();
    ASSERT_GT(file_bytes, control_bytes);
    ASSERT_GT(raw_bytes, control_bytes);
    AuthorityReader reader;
    std::string error;
    ASSERT_TRUE(reader.admit(save.path(), WindowLimits(), error)) << error;
    const std::array fields{&AuthorityLimits::records,
                            &AuthorityLimits::headers,
                            &AuthorityLimits::compressed_member_bytes,
                            &AuthorityLimits::compressed_total_bytes,
                            &AuthorityLimits::decoded_member_bytes,
                            &AuthorityLimits::decoded_total_bytes};
    const std::array<std::uint64_t, 6> caps{1,
                                            5,
                                            file_bytes - 1,
                                            control_bytes + file_bytes - 1,
                                            raw_bytes - 1,
                                            control_bytes + raw_bytes - 1};
    for (std::size_t cap = 0; cap < fields.size(); ++cap) {
        auto limits = WindowLimits();
        limits.*fields[cap] = caps[cap];
        const auto decodes = WorldSaveService::chunk_record_decode_count_for_testing();
        const auto result = reader.read({16, 16, 16, 16}, limits);
        ExpectWindowUnavailable(result);
        EXPECT_NE(result.error.find("limit"), std::string::npos) << result.error;
        EXPECT_EQ(WorldSaveService::chunk_record_decode_count_for_testing(), decodes);
    }
    EXPECT_EQ(reader.read({16, 16, 16, 16}, WindowLimits()).membership,
              AuthorityMembership::Present);
}

TEST(WorldSaveService, AuthorityWindowRefusesWrongTypeAndUnexpectedAuthority) {
    for (const bool wrong_type : {false, true}) {
        const TempSaveDir save("authority_window_inventory");
        ASSERT_NO_FATAL_FAILURE(WriteWindowWorld(save.path(), {}));
        AuthorityReader reader;
        std::string error;
        ASSERT_TRUE(reader.admit(save.path(), WindowLimits(), error)) << error;
        const auto path = WorldSaveService::region_file_path(save.path(), 0, 0);
        if (wrong_type) {
            std::filesystem::create_directory(path);
        } else {
            WorldStreamingState state;
            const auto chunk = AddWindowChunk(state, {0, 0, 0});
            // Copy a valid member without publishing another committed index.
            const TempSaveDir other("authority_window_unlisted");
            ASSERT_NO_FATAL_FAILURE(WriteWindowWorld(other.path(), state, false));
            std::filesystem::copy_file(WorldSaveService::region_file_path(other.path(), 0, 0),
                                       path);
            EXPECT_TRUE(chunk->has_terrain_authority());
        }
        ExpectWindowUnavailable(reader.read({}, WindowLimits()));
    }
}

TEST(WorldSaveService, AuthorityWindowRejectsUncommittedAndMismatchedControlInputs) {
    namespace P = Luminumbra::Persistence;
    const TempSaveDir save("authority_window_controls");
    ASSERT_NO_FATAL_FAILURE(WriteWindowWorld(save.path(), {}));
    AuthorityReader reader;
    std::string error;
    ASSERT_TRUE(reader.admit(save.path(), WindowLimits(), error)) << error;
    {
        P::SaveTransaction transaction(save.path(), 0, {}, WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(nullptr));
        EXPECT_FALSE(reader.admit(save.path(), WindowLimits(), error));
        ExpectWindowUnavailable(reader.read({}, WindowLimits()));
        transaction.abort(nullptr);
    }
    ASSERT_TRUE(reader.admit(save.path(), WindowLimits(), error)) << error;
    const auto manifest = WorldSaveService::world_manifest_path(save.path());
    const auto bytes = ReadFileBytes(manifest);
    ASSERT_NO_FATAL_FAILURE(ReplaceWindowBytes(manifest, bytes + " "));
    EXPECT_FALSE(reader.admit(save.path(), WindowLimits(), error));
    EXPECT_NE(error.find("membership"), std::string::npos) << error;
    ASSERT_NO_FATAL_FAILURE(ReplaceWindowBytes(manifest, bytes));
    ASSERT_TRUE(reader.admit(save.path(), WindowLimits(), error)) << error;
    ASSERT_TRUE(std::filesystem::remove(P::snapshot_index_path(save.path())));
    EXPECT_FALSE(reader.admit(save.path(), WindowLimits(), error));
    EXPECT_NE(error.find("index missing"), std::string::npos) << error;
}

TEST(WorldSaveService, AuthorityWindowLegacyEmptyMissingAndUnknownInventoryStayDistinct) {
    const TempSaveDir save("authority_window_legacy_empty");
    AuthorityReader reader;
    std::string error;
    ASSERT_TRUE(reader.admit(save.path(), WindowLimits(), error)) << error;
    EXPECT_EQ(reader.read({}, WindowLimits()).membership, AuthorityMembership::ProvenAbsent);
    std::filesystem::create_directories(WorldSaveService::region_directory(save.path()));
    const auto index = Luminumbra::Persistence::snapshot_index_path(save.path());
    ASSERT_NO_FATAL_FAILURE(ReplaceWindowBytes(index, "{"));
    EXPECT_FALSE(reader.admit(save.path(), WindowLimits(), error));
    ASSERT_TRUE(std::filesystem::remove(index));
    std::filesystem::create_directory(Luminumbra::Persistence::journal_directory(save.path()));
    EXPECT_FALSE(reader.admit(save.path(), WindowLimits(), error));
    ExpectWindowUnavailable(reader.read({}, WindowLimits()));
}

TEST(WorldSaveService, AuthorityWindowPinsRootAndAdmitsExplicitlyDryWater) {
    const TempSaveDir save("authority_window_root");
    const auto root = save.path() / "world";
    std::filesystem::create_directory(root);
    WorldStreamingState state;
    const auto chunk = AddWindowChunk(state, {0, 0, 0}, true);
    chunk->water_depth_mm.assign(64, 0);
    ASSERT_NO_FATAL_FAILURE(WriteWindowWorld(root, state));
    AuthorityReader reader;
    std::string error;
    ASSERT_TRUE(reader.admit(root, WindowLimits(), error)) << error;
    const auto result = reader.read({}, WindowLimits());
    ASSERT_EQ(result.membership, AuthorityMembership::Present) << result.error;
    ASSERT_EQ(result.chunks.size(), 1u);
    EXPECT_EQ(result.chunks.front()->water_depth_mm, chunk->water_depth_mm);
    std::filesystem::rename(root, save.path() / "retained");
    std::filesystem::create_directory(root);
    ExpectWindowUnavailable(reader.read({}, WindowLimits()));
}

TEST(WorldSaveService, AuthorityWindowAdmissionMemberCapPrecedesInventoryConversion) {
    const TempSaveDir save("authority_window_member_cap");
    WorldStreamingState state;
    AddWindowChunk(state, {0, 0, 0});
    AddWindowChunk(state, {32, 0, 0});
    for (const bool indexed : {false, true}) {
        const auto root = save.path() / (indexed ? "indexed" : "legacy");
        std::filesystem::create_directory(root);
        ASSERT_NO_FATAL_FAILURE(WriteWindowWorld(root, state, indexed));
        AuthorityReader reader;
        std::string error;
        auto limits = WindowLimits();
        limits.members = 1;
        EXPECT_FALSE(reader.admit(root, limits, error));
        EXPECT_NE(error.find("member limit"), std::string::npos) << error;
        ExpectWindowUnavailable(reader.read({}, WindowLimits()));
        ASSERT_TRUE(reader.admit(root, WindowLimits(), error)) << error;
        EXPECT_EQ(reader.read({}, WindowLimits()).membership, AuthorityMembership::Present);
    }
}

TEST(WorldSaveService, AuthorityWindowMalformedInventoryValuesCannotBypassMemberCap) {
    namespace P = Luminumbra::Persistence;
    const TempSaveDir save("authority_window_malformed_inventory");
    std::filesystem::create_directories(WorldSaveService::region_directory(save.path()));
    P::SnapshotIndex index;
    index.generation = 1;
    auto document = nlohmann::json::parse(P::encode_snapshot_index(index));
    auto limits = WindowLimits();
    limits.members = 1;
    for (const auto& value :
         {nlohmann::json(0), nlohmann::json::array(), nlohmann::json::object()}) {
        document["members"] = nlohmann::json::array({value, value});
        ASSERT_NO_FATAL_FAILURE(
            ReplaceWindowBytes(P::snapshot_index_path(save.path()), document.dump()));
        AuthorityReader reader;
        std::string error;
        EXPECT_FALSE(reader.admit(save.path(), limits, error));
        EXPECT_NE(error.find("member limit"), std::string::npos) << error;
    }
}

TEST(WorldSaveService, AuthorityWindowPreservesUnflaggedLegacyWaterWithoutTerrainOwnership) {
    const TempSaveDir save("authority_window_unflagged_water");
    WorldStreamingState state;
    const auto chunk = AddWindowChunk(state, {0, 0, 0}, true);
    chunk->mark_sdf_generated_current_params();
    const WorldSaveService::ContainerRecord record{
        chunk->get_id(), 0, 0, Luminumbra::Persistence::SerializeChunkRecordPayloadJson(*chunk)};
    ASSERT_TRUE(WorldSaveService::upsert_container_records(
        WorldSaveService::region_file_path(save.path(), 0, 0), {record}));
    AuthorityReader reader;
    std::string error;
    ASSERT_TRUE(reader.admit(save.path(), WindowLimits(), error)) << error;
    const auto result = reader.read({}, WindowLimits());
    ASSERT_EQ(result.membership, AuthorityMembership::Present) << result.error;
    ASSERT_EQ(result.chunks.size(), 1u);
    EXPECT_EQ(result.generation, 0u);
    EXPECT_TRUE(result.chunks.front()->has_water_authority());
    EXPECT_FALSE(result.chunks.front()->has_terrain_authority());
    EXPECT_EQ(result.chunks.front()->water_bed_mm, chunk->water_bed_mm);
    EXPECT_EQ(result.chunks.front()->water_depth_mm, chunk->water_depth_mm);
}
