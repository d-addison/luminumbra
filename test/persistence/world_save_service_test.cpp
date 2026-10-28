#include "../fixtures/TerrainFixtureParams.h"
#include "gtest/gtest.h"

#include "../support/CalendarMetadataCases.h"
#include "../support/PersistenceFileOps.h"
#include "../support/UniqueTempPath.h"

#include "core/FilesystemPath.h"
#include "luminumbra/core/Types.h"
#include "persistence/SavedInputFile.h"
#include "persistence/WorldPersistenceRoundtrip.h"
#include "persistence/WorldSaveService.h"
#include "systems/SHIELD_WorldSystem.h"
#include "world/ActiveRegionLedger.h"
#include "world/Chunk.h"
#include "world/MarchingCubes.h"
#include "world/WorldClock.h"
#include "world/WorldStreamingState.h"

#include <array>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <span>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <system_error>
#include <thread>
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
        if (empty)
            ASSERT_TRUE(WorldSaveService::remove_file_and_staging(file));
        else
            ASSERT_TRUE(WorldSaveService::write_file_durably(file, "complete field"));
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

TEST(WorldSaveService, EmptyLedgerAndMetadataSavesReclaimBeforeTheirWriteOrNoop) {
    const TempSaveDir save_dir("empty_ledger_cleanup");
    const auto ledger_staging = save_dir.path() / "active-regions.arl.tmp.abandoned";
    ASSERT_TRUE(WorldSaveService::write_file_durably(ledger_staging, "partial ledger"));
    Luminumbra::world::ActiveRegionLedger ledger;
    const Luminumbra::world::WorldClock clock;
    ASSERT_TRUE(WorldSaveService::save_active_regions(ledger, clock, save_dir.path()));
    EXPECT_FALSE(std::filesystem::exists(ledger_staging));
    EXPECT_FALSE(std::filesystem::exists(WorldSaveService::region_directory(save_dir.path())));
    EXPECT_TRUE(std::filesystem::is_empty(save_dir.path()));

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
    EXPECT_FALSE(std::filesystem::exists(ledger_staging));
    EXPECT_FALSE(std::filesystem::exists(WorldSaveService::region_directory(save_dir.path())));
}

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
    nlohmann::json metadata = {{"container_version", 2}};
    Luminumbra::world::WorldClock(123, {12000, 4}).write_metadata(metadata);
    const auto valid = metadata.dump(4) + "\n";
    ASSERT_TRUE(WorldSaveService::save_metadata(valid, save_dir.path()));
    EXPECT_EQ(read(), valid);
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
    EXPECT_FALSE(std::filesystem::exists(save_dir.path() / "chunks"));
    EXPECT_EQ(read(), damaged);
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
                  root.path(), {0, 0}, RegionDigestInputs{chunks, {}}),
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
    const auto settled = freeze({}, RegionDigestInputs{resident.snapshot_chunks(), {}});
    ASSERT_NE(settled, freeze({}, RegionDigestInputs{}));
    EXPECT_EQ(freeze(root.path(), RegionDigestInputs{resident.snapshot_chunks(), {}}), settled);
    EXPECT_EQ(freeze({}, RegionDigestInputs{{}, resident.snapshot_chunks()}), settled);

    // Incomplete live/parked content must fall back to the edited durable Loading record.
    auto incomplete = std::make_shared<Chunk>(chunk->get_coords());
    incomplete->heightmap_data = {99.0f};
    incomplete->mark_voxel_data_dirty();
    incomplete->set_state(ChunkState::Loading);
    EXPECT_EQ(freeze({}, RegionDigestInputs{{incomplete}, {}}), freeze({}, RegionDigestInputs{}));
    EXPECT_EQ(freeze({}, RegionDigestInputs{{}, {incomplete}}), freeze({}, RegionDigestInputs{}));
    EXPECT_EQ(freeze(root.path(), RegionDigestInputs{{incomplete}, {incomplete}}), settled);

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
                {}, {0, 0}, RegionDigestInputs{{chunk}, {}});
            if (edited)
                ASSERT_NE(settled, empty);
            else
                ASSERT_EQ(settled, empty);
            chunk->set_state(lifecycle);
            const auto in_memory = lifecycle == ChunkState::Loading ? empty : settled;
            EXPECT_EQ(WorldSaveService::region_simulation_digest(
                          {}, {0, 0}, RegionDigestInputs{{chunk}, {}}),
                      in_memory);
            EXPECT_EQ(WorldSaveService::region_simulation_digest(
                          {}, {0, 0}, RegionDigestInputs{{}, {chunk}}),
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
        EXPECT_EQ(WorldSaveService::region_simulation_digest(missing_save, key, inputs),
                  empty_digest);
        WorldStreamingState region;
        const auto chunk =
            AddFixtureChunk(region, IVec3(key.x * 32, 0, key.z * 32), ChunkState::Idle, 1u);
        chunk->mark_voxel_data_dirty();
        const Luminumbra::Persistence::RegionDigestInputs edited_inputs{{chunk}, {}};
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
