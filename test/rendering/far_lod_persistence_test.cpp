#include "../fixtures/TerrainFixtureParams.h"
#include "../support/PersistenceFileOps.h"
#include "../support/UniqueTempPath.h"
#include "gtest/gtest.h"

#include "luminumbra/core/Types.h"
#include "luminumbra_client/rendering/FarLodIntegrationBudget.h"
#include "luminumbra_client/rendering/FarLodSystem.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/ecs/EntitySnapshot.h"
#include "luminumbra_common/persistence/RegionFileWriteGuard.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/ActiveRegionLedger.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/FarLodStore.h"
#include "luminumbra_common/world/WorldClock.h"
#include "luminumbra_common/world/WorldStreamingState.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <latch>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace Luminumbra;
using namespace Luminumbra::World;
using Luminumbra::Persistence::WorldSaveService;
using Luminumbra::Persistence::detail::RegionFileWriteGuard;
using Luminumbra::Rendering::FarLodIntegrationBudget;

class SaveDirectory {
public:
    const std::filesystem::path& path() const {
        return m_path;
    }
    ~SaveDirectory() {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

private:
    std::filesystem::path m_path = TestSupport::CreateUniqueTempDirectory("luminumbra_far_async_");
};

std::string Bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

FarLodTile Tile(FarLodTier tier, u16 height) {
    FarLodTile tile;
    tile.tier = tier;
    tile.rx = -1;
    tile.rz = 2;
    tile.params_hash = 1234;
    tile.samples_per_side = FarLodSamplesPerSide(tier);
    tile.height_q.assign(tile.sample_count(), height);
    tile.material.assign(tile.sample_count(), 7u);
    tile.flags.assign(tile.sample_count(), kFarLodSampleFlagWater);
    return tile;
}

// The owner holds a real writer transaction, so a competing writer cannot
// pass acquisition until the test releases it. This const diagnostic confirms
// that the worker reached acquisition; there is no sleep or scheduling wager.
void WaitForCompetingWriter(const RegionFileWriteGuard& transaction) {
    while (transaction.waiting_writers() == 0)
        std::this_thread::yield();
}

void ExpectNoTemps(const std::filesystem::path& root) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
        EXPECT_EQ(entry.path().filename().string().find(".tmp."), std::string::npos);
}

enum class SiblingSave : std::uint8_t {
    Metadata,
    Ledger,
    Plants,
    DeletePlants,
    RewindClock
};

struct FileOverlap {
    std::filesystem::path path;
    std::filesystem::path blocking_path;
    std::thread::id validator{};
    std::latch opened{1};
    std::latch release{1};
    std::atomic<bool> block_first_read{true};
    std::atomic<unsigned> readers{0};
    std::atomic<unsigned> publications{0};
    std::atomic<unsigned> sharing_violations{0};
    bool refuse_removal = false;
    bool refuse_replacement = false;
    unsigned validator_reads_to_skip = 0;

    static FileOverlap* active;
    static void ObserveRead(const std::filesystem::path& target, bool opening) {
        auto& overlap = *active;
        if (target == overlap.path) {
            if (opening)
                overlap.readers.fetch_add(1);
            else
                overlap.readers.fetch_sub(1);
        }
        if (opening && target == overlap.blocking_path &&
            std::this_thread::get_id() == overlap.validator) {
            if (overlap.validator_reads_to_skip != 0) {
                --overlap.validator_reads_to_skip;
                return;
            }
            if (overlap.block_first_read.exchange(false)) {
                overlap.opened.count_down();
                overlap.release.wait();
            }
        }
    }
    static bool Publish(const std::filesystem::path& target, bool removing) {
        auto& overlap = *active;
        if (target != overlap.path)
            return true;
        overlap.publications.fetch_add(1);
        // Emulate Windows' refusal to replace/delete an open read stream even
        // on POSIX, where rename/unlink would otherwise hide the regression.
        if (overlap.readers.load() != 0) {
            overlap.sharing_violations.fetch_add(1);
            return false;
        }
        return removing ? !overlap.refuse_removal : !overlap.refuse_replacement;
    }
};
FileOverlap* FileOverlap::active = nullptr;

class FileOverlapGuard {
public:
    explicit FileOverlapGuard(FileOverlap& overlap) {
        FileOverlap::active = &overlap;
        Persistence::detail::set_file_read_observer_for_testing(&FileOverlap::ObserveRead);
        Persistence::detail::set_file_publication_hook_for_testing(&FileOverlap::Publish);
    }
    ~FileOverlapGuard() {
        Persistence::detail::set_file_read_observer_for_testing(nullptr);
        Persistence::detail::set_file_publication_hook_for_testing(nullptr);
        FileOverlap::active = nullptr;
    }
    FileOverlapGuard(const FileOverlapGuard&) = delete;
    FileOverlapGuard& operator=(const FileOverlapGuard&) = delete;
};

// Admission transfers the payload only on success. Keep the test's lvalue so
// refusal assertions can inspect every byte without an unconditional move.
bool TryEnqueue(AsyncFarLodStore& writer, const std::filesystem::path& path, FarLodTile& tile) {
    return writer.enqueue(path, std::move(tile));
}

Ecs::EntityRegistrySnapshot Plants(const std::string& name) {
    Ecs::EntityRegistrySnapshot plants;
    plants.entities.push_back(
        Ecs::EntitySnapshotRecord{.entity_id = 1, .name = name, .components = {}});
    return plants;
}

void ExpectSiblingOverlap(SiblingSave operation) {
    const SaveDirectory world;
    const WorldStreamingState state;
    const WorldSaveService service;
    ASSERT_TRUE(service.save_world(state, world.path()));
    const world::WorldClock clock(100);
    nlohmann::json metadata = {{"container_version", WorldSaveService::kContainerVersion}};
    clock.write_metadata(metadata);
    world::ActiveRegionLedger ledger({}, clock);
    ledger.set_local_anchor(Vec3(1, 2, 3));
    ASSERT_TRUE(
        WorldSaveService::save_metadata_and_active_regions(metadata.dump(), ledger, world.path()));
    ASSERT_TRUE(WorldSaveService::save_plant_entities(Plants("before"), world.path()));
    const auto target = operation == SiblingSave::Metadata ? world.path() / "world_info.json"
                        : operation == SiblingSave::Ledger || operation == SiblingSave::RewindClock
                            ? WorldSaveService::active_regions_path(world.path())
                            : WorldSaveService::plant_entities_path(world.path());
    const auto before = Bytes(target);
    FileOverlap overlap{.path = target, .blocking_path = target};
    const FileOverlapGuard observing(overlap);
    if (operation == SiblingSave::RewindClock) {
        // Pause after reading the ledger, with metadata open for the second
        // (pair comparison) read. The ledger stream is already closed, so only
        // the pair gate can block this rewind.
        overlap.blocking_path = world.path() / "world_info.json";
        overlap.validator_reads_to_skip = 1;
    }
    std::vector<std::string> validation_errors;
    std::vector<std::string> save_errors;
    bool validated = false;
    bool saved = false;
    std::atomic<bool> writer_done{false};
    std::jthread validator([&] {
        overlap.validator = std::this_thread::get_id();
        validated = WorldSaveService::validate_save(world.path(), &validation_errors);
    });
    overlap.opened.wait();
    std::jthread writer([&] {
        switch (operation) {
            case SiblingSave::Metadata:
                metadata["name"] = "after";
                saved =
                    WorldSaveService::save_metadata(metadata.dump(), world.path(), &save_errors);
                break;
            case SiblingSave::Ledger:
                ledger.set_local_anchor(Vec3(4, 5, 6));
                saved = WorldSaveService::save_active_regions(
                    ledger, clock, world.path(), &save_errors);
                break;
            case SiblingSave::Plants:
                saved = WorldSaveService::save_plant_entities(
                    Plants("after"), world.path(), &save_errors);
                break;
            case SiblingSave::DeletePlants:
                saved = WorldSaveService::save_plant_entities({}, world.path(), &save_errors);
                break;
            case SiblingSave::RewindClock: {
                const world::WorldClock rewound(50);
                world::ActiveRegionLedger restored({}, rewound);
                restored.set_local_anchor(Vec3(4, 5, 6));
                rewound.write_metadata(metadata);
                saved = WorldSaveService::save_metadata_and_active_regions(
                    metadata.dump(), restored, world.path(), &save_errors);
                break;
            }
        }
        writer_done.store(true);
    });
    // Release only after a failed try_lock proves actual reader/writer overlap,
    // or after an unprotected publication attempt exposes the sharing violation.
    while (Persistence::detail::waiting_file_publishers_for_testing(target) == 0 &&
           overlap.publications.load() == 0 && !writer_done.load())
        std::this_thread::yield();
    EXPECT_GT(Persistence::detail::waiting_file_publishers_for_testing(target), 0u);
    EXPECT_EQ(overlap.publications.load(), 0u);
    overlap.release.count_down();
    validator.join();
    writer.join();
    EXPECT_TRUE(validated);
    EXPECT_TRUE(saved);
    EXPECT_TRUE(validation_errors.empty());
    EXPECT_TRUE(save_errors.empty());
    EXPECT_EQ(overlap.sharing_violations.load(), 0u);
    EXPECT_EQ(overlap.publications.load(), 1u);
    EXPECT_EQ(overlap.readers.load(), 0u);
    if (operation == SiblingSave::DeletePlants) {
        EXPECT_FALSE(std::filesystem::exists(target));
    } else {
        EXPECT_NE(Bytes(target), before);
    }
    EXPECT_TRUE(WorldSaveService::validate_save(world.path()));
    ExpectNoTemps(world.path());
}
} // namespace

TEST(FarLodPersistence, ValidationMetadataStreamClosesBeforeSaveReplacement) {
    ExpectSiblingOverlap(SiblingSave::Metadata);
}

TEST(FarLodPersistence, ValidationLedgerStreamClosesBeforeSaveReplacement) {
    ExpectSiblingOverlap(SiblingSave::Ledger);
}

TEST(FarLodPersistence, ValidationPlantStreamClosesBeforeSaveReplacement) {
    ExpectSiblingOverlap(SiblingSave::Plants);
}

TEST(FarLodPersistence, ValidationPlantStreamClosesBeforeSaveDeletion) {
    ExpectSiblingOverlap(SiblingSave::DeletePlants);
}

TEST(FarLodPersistence, ValidationClockLedgerComparisonExcludesConcurrentRewind) {
    ExpectSiblingOverlap(SiblingSave::RewindClock);
}

TEST(FarLodPersistence, PlantReplacementFailurePreservesAuthorityAndRemovesStaging) {
    const SaveDirectory world;
    ASSERT_TRUE(WorldSaveService::save_plant_entities(Plants("retained"), world.path()));
    const auto path = WorldSaveService::plant_entities_path(world.path());
    const auto before = Bytes(path);
    FileOverlap overlap{.path = path, .blocking_path = path};
    const FileOverlapGuard observing(overlap);
    overlap.refuse_replacement = true;
    std::vector<std::string> errors;
    EXPECT_FALSE(WorldSaveService::save_plant_entities(Plants("refused"), world.path(), &errors));
    EXPECT_FALSE(errors.empty());
    EXPECT_EQ(overlap.publications.load(), 1u);
    EXPECT_EQ(Bytes(path), before);
    ExpectNoTemps(world.path());
}

TEST(FarLodPersistence, PlantDeletionFailureReportsFailureAndRetainsAuthority) {
    const SaveDirectory world;
    ASSERT_TRUE(WorldSaveService::save_plant_entities(Plants("retained"), world.path()));
    const auto path = WorldSaveService::plant_entities_path(world.path());
    const auto before = Bytes(path);
    {
        FileOverlap overlap{.path = path, .blocking_path = path};
        const FileOverlapGuard observing(overlap);
        overlap.refuse_removal = true;
        std::vector<std::string> errors;
        EXPECT_FALSE(WorldSaveService::save_plant_entities({}, world.path(), &errors));
        ASSERT_FALSE(errors.empty());
        EXPECT_NE(errors.back().find("failed to remove plant entities file"), std::string::npos);
        EXPECT_EQ(overlap.publications.load(), 1u);
        EXPECT_EQ(Bytes(path), before);
    }
    Ecs::EntityRegistrySnapshot loaded;
    ASSERT_TRUE(WorldSaveService::load_plant_entities(loaded, world.path()));
    EXPECT_EQ(Ecs::SerializeEntityRegistrySnapshotJson(loaded), before);
    ASSERT_TRUE(WorldSaveService::save_plant_entities({}, world.path()));
    EXPECT_FALSE(std::filesystem::exists(path));
    ExpectNoTemps(world.path());
}

TEST(FarLodPersistence, AsyncBytesMatchSynchronousIncludingOrderedSameTileWrites) {
    const SaveDirectory synchronous;
    const SaveDirectory asynchronous;
    AsyncFarLodStore writer;
    std::vector<std::string> errors;
    // Interleaved tiers share one region container. Repeated writes to the same
    // key must leave the last payload, with exactly the synchronous file bytes.
    for (const u16 height : {u16{100}, u16{200}, u16{300}}) {
        for (const FarLodTier tier : {FarLodTier::F1, FarLodTier::F2}) {
            FarLodTile tile = Tile(tier, height);
            FarLodSdfSnapshot source;
            source.coords = IVec3(-1, 0, 64);
            source.revision = height;
            constexpr std::size_t kSamples = static_cast<std::size_t>(CHUNK_SIZE_X + 1) *
                                             (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
            source.sdf_data.assign(kSamples, -1.0f);
            source.material_data.assign(kSamples, 11u);
            ASSERT_EQ(ReduceChunkSdfIntoFarTile(tile, source), FarLodSdfReduceResult::Inserted);
            ASSERT_TRUE(FarLodStore(synchronous.path()).save_tile(tile, &errors));
            ASSERT_TRUE(writer.enqueue(asynchronous.path(), std::move(tile)));
        }
    }
    writer.flush();
    const auto diagnostics = writer.diagnostics();
    EXPECT_EQ(diagnostics.saved, 6u);
    EXPECT_EQ(diagnostics.pending, 0u);
    EXPECT_EQ(diagnostics.failed, 0u);
    const auto relative = std::filesystem::path("chunks/region/r.-1.2.lmr");
    ASSERT_TRUE(std::filesystem::exists(asynchronous.path() / relative));
    EXPECT_EQ(Bytes(asynchronous.path() / relative), Bytes(synchronous.path() / relative));
    ExpectNoTemps(asynchronous.path());
}

TEST(FarLodPersistence, FirstWorkerSaveAndFlushCompleteWithinBoundedWait) {
    // Run in a child so a lock regression fails promptly without joining a
    // deadlocked worker during test cleanup. No native test timeout is raised.
    ASSERT_EXIT(
        {
            {
                const SaveDirectory world;
                const auto params = Luminumbra::Test::SyntheticTerrainParams();
                Systems::SHIELD_WorldSystem authority(nullptr, nullptr, params, 1337);
                bool completed = false;
                {
                    AsyncFarLodStore writer;
                    const auto revision = authority.far_lod_authority_revision();
                    if (!writer.enqueue(
                            world.path(), Tile(FarLodTier::F1, 100), &authority, revision) ||
                        !writer.enqueue(
                            world.path(), Tile(FarLodTier::F2, 200), &authority, revision))
                        std::_Exit(1);
                    // Two tiles bound the workload; the deadline bounds polling and
                    // terminates even if a worker re-acquires a non-recursive lock.
                    const auto deadline =
                        std::chrono::steady_clock::now() + std::chrono::seconds(3);
                    while (writer.diagnostics().pending != 0 &&
                           std::chrono::steady_clock::now() < deadline)
                        std::this_thread::yield();
                    if (writer.diagnostics().pending != 0)
                        std::_Exit(2);
                    writer.flush();
                    writer.close();
                    const auto result = writer.diagnostics();
                    completed = result.saved == 2 && result.failed == 0 && result.closed;
                }
                // Leave the fixture scope before successful exit to remove its files.
                if (!completed)
                    std::_Exit(3);
            }
            std::_Exit(0);
        },
        ::testing::ExitedWithCode(0),
        "");
}

TEST(FarLodPersistence, QueuedTilesRecheckAuthorityAfterAcquiringPersistenceOwnership) {
    // Both tiers and both absent/present durable tiles must refuse an obsolete
    // capture. Zero is a valid initial authority revision, never an unknown token.
    for (const auto tier : {FarLodTier::F1, FarLodTier::F2}) {
        for (const bool publish_newer : {false, true}) {
            SCOPED_TRACE(static_cast<unsigned>(tier));
            SCOPED_TRACE(publish_newer);
            const SaveDirectory world;
            const auto params = Luminumbra::Test::SyntheticTerrainParams();
            Systems::SHIELD_WorldSystem authority(nullptr, nullptr, params, 1337);
            AsyncFarLodStore writer;
            const auto path = WorldSaveService::region_file_path(world.path(), -1, 2);
            FarLodSdfSnapshot source;
            source.coords = IVec3(-1, 0, 64);
            source.revision = 1;
            constexpr std::size_t kSamples = static_cast<std::size_t>(CHUNK_SIZE_X + 1) *
                                             (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
            source.sdf_data.assign(kSamples, -1.0f);
            source.material_data.assign(kSamples, 11u);
            auto obsolete = Tile(tier, 100);
            ASSERT_EQ(ReduceChunkSdfIntoFarTile(obsolete, source), FarLodSdfReduceResult::Inserted);
            auto newer = Tile(tier, 200);
            source.revision = 2;
            source.sdf_data.assign(kSamples, -2.0f);
            ASSERT_EQ(ReduceChunkSdfIntoFarTile(newer, source), FarLodSdfReduceResult::Inserted);
            std::string newer_bytes;
            {
                const RegionFileWriteGuard transaction(path);
                ASSERT_TRUE(writer.enqueue(world.path(),
                                           std::move(obsolete),
                                           &authority,
                                           authority.far_lod_authority_revision()));
                WaitForCompetingWriter(transaction);
                if (publish_newer) {
                    WorldStreamingState state;
                    auto chunk = state.get_or_create_chunk(IVec3(-1, 0, 64));
                    chunk->sdf_data = {-2.0f, 3.0f};
                    chunk->material_data = {7u, 11u};
                    chunk->mark_voxel_data_dirty();
                    ASSERT_TRUE(WorldSaveService{}.save_world(state, world.path()));
                    ASSERT_TRUE(FarLodStore(world.path()).save_tile(newer));
                    newer_bytes = Bytes(path);
                }
                // Matches the session's durable notification while it still
                // owns the complete save, including partial-failure attempts.
                authority.notify_far_lod_authority_durable(IVec3(-1, 0, 64));
            }
            writer.flush();
            EXPECT_EQ(writer.diagnostics().failed, 1u);
            EXPECT_EQ(writer.diagnostics().saved, 0u);
            EXPECT_EQ(writer.diagnostics().pending, 0u);
            if (publish_newer)
                EXPECT_EQ(Bytes(path), newer_bytes);
            else
                EXPECT_TRUE(std::filesystem::is_empty(world.path()));
            // A fresh capture can still commit and drain after stale rejection.
            auto fresh = Tile(tier, 300);
            ASSERT_EQ(ReduceChunkSdfIntoFarTile(fresh, source), FarLodSdfReduceResult::Inserted);
            ASSERT_TRUE(writer.enqueue(world.path(),
                                       std::move(fresh),
                                       &authority,
                                       authority.far_lod_authority_revision()));
            writer.close();
            EXPECT_EQ(writer.diagnostics().saved, 1u);
            EXPECT_TRUE(writer.diagnostics().closed);
            FarLodTile loaded;
            ASSERT_TRUE(FarLodStore(world.path()).load_tile(tier, -1, 2, 1234, loaded));
            EXPECT_EQ(loaded.height_q.front(), 300u);
            EXPECT_TRUE(loaded.edited);
            ASSERT_EQ(loaded.sdf_bricks.size(), 1u);
            EXPECT_EQ(loaded.sdf_bricks.front().revision, 2u);
            ExpectNoTemps(world.path());
        }
    }
}

namespace {
struct AuthorityPublicationOverlap {
    Systems::SHIELD_WorldSystem& authority;
    std::filesystem::path path;
    bool refuse_publication = false;
    std::atomic<bool> bumped{false};
    bool bump_blocked = false;
    u64 publication_revision = 0;
    std::jthread editor{};

    static AuthorityPublicationOverlap*& Active() {
        static AuthorityPublicationOverlap* overlap = nullptr;
        return overlap;
    }

    static bool Publish(const std::filesystem::path& target, bool removing) {
        auto& overlap = *Active();
        if (removing || target != overlap.path)
            return true;
        // The tile's initial revision check has passed and staging is durable.
        // Attempt the edit on another thread: a same-thread call would merely
        // exercise recursive ownership and could not prove mutual exclusion.
        overlap.editor = std::jthread([&overlap] {
            overlap.authority.notify_far_lod_authority_durable(IVec3(-1, 0, 64));
            overlap.bumped.store(true, std::memory_order_release);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!overlap.bumped.load(std::memory_order_acquire) &&
               WorldSaveService::waiting_save_writers() == 0 &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        overlap.bump_blocked = WorldSaveService::waiting_save_writers() == 1 &&
                               !overlap.bumped.load(std::memory_order_acquire);
        overlap.publication_revision = overlap.authority.far_lod_authority_revision();
        return !overlap.refuse_publication;
    }
};

void CheckAuthorityPublication(FarLodTier tier, bool existing, bool refuse, bool closing) {
    const SaveDirectory world;
    const auto params = Luminumbra::Test::SyntheticTerrainParams();
    Systems::SHIELD_WorldSystem authority(nullptr, nullptr, params, 1337);
    const auto path = WorldSaveService::region_file_path(world.path(), -1, 2);
    if (existing)
        ASSERT_TRUE(FarLodStore(world.path()).save_tile(Tile(tier, 50)));
    const auto before = Bytes(path);
    const auto revision = authority.far_lod_authority_revision();
    EXPECT_EQ(revision, 0u); // Zero is a valid captured revision.
    auto tile = Tile(tier, 100);
    FarLodSdfSnapshot source;
    source.coords = IVec3(-1, 0, 64);
    source.revision = 1;
    // One full chunk lattice bounds the source and reduced SDF streams.
    constexpr std::size_t kSamples =
        static_cast<std::size_t>(CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
    source.sdf_data.assign(kSamples, -1.0f);
    source.material_data.assign(kSamples, 11u);
    ASSERT_EQ(ReduceChunkSdfIntoFarTile(tile, source), FarLodSdfReduceResult::Inserted);
    const auto fresh = tile;
    AsyncFarLodStore writer;
    AuthorityPublicationOverlap overlap{
        .authority = authority, .path = path, .refuse_publication = refuse};
    AuthorityPublicationOverlap::Active() = &overlap;
    Persistence::detail::set_file_publication_hook_for_testing(
        &AuthorityPublicationOverlap::Publish);
    if (!writer.enqueue(world.path(), std::move(tile), &authority, revision))
        std::_Exit(1);
    std::atomic<bool> drained{false};
    std::jthread drain([&] {
        if (!closing)
            writer.flush();
        writer.close();
        drained.store(true, std::memory_order_release);
    });
    // Bound both lifecycle waits and editor cleanup, including a lock inversion.
    // No sleep or unbounded join can mask a regression in the child process.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((!drained.load(std::memory_order_acquire) ||
            !overlap.bumped.load(std::memory_order_acquire)) &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    if (!drained.load(std::memory_order_acquire) || !overlap.bumped.load(std::memory_order_acquire))
        std::_Exit(2);
    drain.join();
    overlap.editor.join();
    Persistence::detail::set_file_publication_hook_for_testing(nullptr);
    AuthorityPublicationOverlap::Active() = nullptr;
    EXPECT_TRUE(overlap.bump_blocked);
    EXPECT_EQ(overlap.publication_revision, revision);
    EXPECT_EQ(authority.far_lod_authority_revision(), revision + 1u);
    EXPECT_EQ(writer.diagnostics().saved, refuse ? 0u : 1u);
    EXPECT_EQ(writer.diagnostics().failed, refuse ? 1u : 0u);
    EXPECT_EQ(writer.diagnostics().pending, 0u);
    EXPECT_TRUE(writer.diagnostics().closed);
    FarLodTile loaded;
    if (refuse) {
        EXPECT_EQ(Bytes(path), before);
        EXPECT_EQ(std::filesystem::exists(path), existing);
    } else {
        // Authoritative tiles intentionally bypass the params hash. Publication
        // itself must therefore be ordered before the authority change.
        ASSERT_TRUE(FarLodStore(world.path()).load_tile(tier, -1, 2, 9999, loaded));
        EXPECT_EQ(loaded.height_q.front(), 100u);
        EXPECT_TRUE(loaded.edited);
    }
    ExpectNoTemps(world.path());
    // Session saves notify while already owning persistence. Bound that nested
    // acquisition as well, so replacing recursive ownership cannot hang cleanup.
    std::atomic<bool> nested_notified{false};
    std::jthread notification([&] {
        const WorldSaveService::SaveOwnership ownership;
        authority.notify_far_lod_authority_durable(IVec3(-1, 0, 64));
        nested_notified.store(true, std::memory_order_release);
    });
    const auto nested_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!nested_notified.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < nested_deadline)
        std::this_thread::yield();
    if (!nested_notified.load(std::memory_order_acquire))
        std::_Exit(2);
    notification.join();
    EXPECT_EQ(authority.far_lod_authority_revision(), revision + 2u);
    const auto published = Bytes(path);
    EXPECT_FALSE(FarLodStore(world.path()).save_tile(fresh, nullptr, &authority, revision));
    EXPECT_EQ(Bytes(path), published);
    EXPECT_TRUE(FarLodStore(world.path())
                    .save_tile(fresh, nullptr, &authority, authority.far_lod_authority_revision()));
    ExpectNoTemps(world.path());
}
} // namespace

TEST(FarLodPersistence, AuthorityBumpCannotCrossTilePublication) {
    ASSERT_EXIT(
        {
            // Sixteen bounded cases cover both tiers, first/replacement writes,
            // failed publication, and flush/shutdown draining an active worker.
            for (const auto tier : {FarLodTier::F1, FarLodTier::F2})
                for (const bool existing : {false, true})
                    for (const bool refuse : {false, true})
                        for (const bool closing : {false, true}) {
                            CheckAuthorityPublication(tier, existing, refuse, closing);
                            if (::testing::Test::HasFailure())
                                std::_Exit(3);
                        }
            std::_Exit(0);
        },
        ::testing::ExitedWithCode(0),
        "");
}

TEST(FarLodPersistence, CurrentAuthorityStillValidatesPayloadAndSavedInputBeforePublishing) {
    const SaveDirectory world;
    const auto params = Luminumbra::Test::SyntheticTerrainParams();
    Systems::SHIELD_WorldSystem authority(nullptr, nullptr, params, 1337);
    AsyncFarLodStore writer;
    const auto revision = authority.far_lod_authority_revision();
    EXPECT_EQ(revision, 0u);
    const auto path = WorldSaveService::region_file_path(world.path(), -1, 2);
    ASSERT_TRUE(writer.enqueue(world.path(), Tile(FarLodTier::F1, 100), &authority, revision));
    writer.flush();
    ASSERT_EQ(writer.diagnostics().saved, 1u);
    const auto before = Bytes(path);
    auto malformed = Tile(FarLodTier::F1, 200);
    malformed.material.pop_back();
    ASSERT_TRUE(writer.enqueue(world.path(), std::move(malformed), &authority, revision));
    writer.flush();
    EXPECT_EQ(writer.diagnostics().failed, 1u);
    EXPECT_EQ(Bytes(path), before);
    // Both an unreadable directory and partial container bytes must retain the
    // durable input, even when the captured authority itself is still current.
    ASSERT_TRUE(std::filesystem::remove(path));
    ASSERT_TRUE(std::filesystem::create_directory(path));
    ASSERT_TRUE(writer.enqueue(world.path(), Tile(FarLodTier::F1, 300), &authority, revision));
    writer.flush();
    EXPECT_EQ(writer.diagnostics().failed, 2u);
    EXPECT_TRUE(std::filesystem::is_directory(path));
    ASSERT_TRUE(std::filesystem::remove(path));
    ASSERT_TRUE(WorldSaveService::write_file_durably(path, "LMR"));
    ASSERT_TRUE(writer.enqueue(world.path(), Tile(FarLodTier::F1, 400), &authority, revision));
    writer.close();
    EXPECT_EQ(writer.diagnostics().failed, 3u);
    EXPECT_EQ(writer.diagnostics().saved, 1u);
    EXPECT_EQ(Bytes(path), "LMR");
    EXPECT_EQ(writer.diagnostics().pending, 0u);
    EXPECT_TRUE(writer.diagnostics().closed);
    ExpectNoTemps(world.path());
}

TEST(FarLodPersistence, WorldBoundaryDrainsCapturedPathsAndCloseRejectsNewWork) {
    const SaveDirectory old_world;
    const SaveDirectory new_world;
    AsyncFarLodStore writer;
    // All accepted saves are drained at the world boundary. No assertion depends
    // on how far the worker has progressed when the owner requests that drain.
    for (const u16 height : {u16{100}, u16{200}, u16{300}, u16{400}})
        ASSERT_TRUE(writer.enqueue(old_world.path(), Tile(FarLodTier::F1, height)));
    writer.flush();
    const auto path = WorldSaveService::region_file_path(old_world.path(), -1, 2);
    const std::string old_bytes = Bytes(path);
    ASSERT_FALSE(old_bytes.empty());
    EXPECT_TRUE(std::filesystem::is_empty(new_world.path()));
    ASSERT_TRUE(writer.enqueue(new_world.path(), Tile(FarLodTier::F2, 999)));
    writer.close(); // drains accepted new-world work before reporting closure
    EXPECT_EQ(writer.diagnostics().pending, 0u);
    EXPECT_EQ(writer.diagnostics().saved, 5u);
    EXPECT_EQ(writer.diagnostics().failed, 0u);
    EXPECT_TRUE(writer.diagnostics().closed);
    FarLodTile refused = Tile(FarLodTier::F1, 555);
    EXPECT_FALSE(TryEnqueue(writer, new_world.path(), refused));
    EXPECT_EQ(refused.height_q.front(), 555u);
    EXPECT_EQ(Bytes(path), old_bytes);
    FarLodTile loaded;
    ASSERT_TRUE(FarLodStore(old_world.path()).load_tile(FarLodTier::F1, -1, 2, 1234, loaded));
    EXPECT_EQ(loaded.height_q.front(), 400u);
    EXPECT_FALSE(FarLodStore(old_world.path()).load_tile(FarLodTier::F2, -1, 2, 1234, loaded));
    ASSERT_TRUE(FarLodStore(new_world.path()).load_tile(FarLodTier::F2, -1, 2, 1234, loaded));
    EXPECT_EQ(loaded.height_q.front(), 999u);
    EXPECT_FALSE(FarLodStore(new_world.path()).load_tile(FarLodTier::F1, -1, 2, 1234, loaded));
    ExpectNoTemps(old_world.path());
    ExpectNoTemps(new_world.path());
}

TEST(FarLodPersistence, RejectedMalformedTileLeavesNoStagingAndDoesNotPreventLaterDrain) {
    const SaveDirectory world;
    AsyncFarLodStore writer;
    const auto path = WorldSaveService::region_file_path(world.path(), -1, 2);
    ASSERT_TRUE(FarLodStore(world.path()).save_tile(Tile(FarLodTier::F1, 100)));
    const auto before = Bytes(path);
    FarLodTile malformed = Tile(FarLodTier::F1, 200);
    malformed.material.pop_back();
    ASSERT_TRUE(writer.enqueue(world.path(), std::move(malformed)));
    writer.flush();
    EXPECT_EQ(writer.diagnostics().failed, 1u);
    EXPECT_EQ(Bytes(path), before);
    ExpectNoTemps(world.path());
    ASSERT_TRUE(writer.enqueue(world.path(), Tile(FarLodTier::F1, 300)));
    writer.close();
    EXPECT_EQ(writer.diagnostics().saved, 1u);
    ExpectNoTemps(world.path());
}

TEST(FarLodPersistence, AsyncTileAndChunkMergesPreserveAuthorityAndSimulationHash) {
    const SaveDirectory world;
    WorldStreamingState state;
    auto chunk = state.get_or_create_chunk(IVec3(-1, 0, 64));
    chunk->sdf_data = {-1.0f, 2.0f};
    chunk->material_data = {7u, 11u};
    chunk->mark_voxel_data_dirty();
    auto unrelated = state.get_or_create_chunk(IVec3(40, 0, 0));
    unrelated->sdf_data = {-3.0f, 4.0f};
    unrelated->material_data = {9u, 13u};
    unrelated->mark_voxel_data_dirty();
    const WorldSaveService service;
    ASSERT_TRUE(service.save_world(state, world.path()));
    const auto unrelated_hash =
        WorldSaveService::region_simulation_digest(world.path(), {1, 0}, {});
    const auto path = WorldSaveService::region_file_path(world.path(), -1, 2);
    AsyncFarLodStore writer;
    for (const u16 height : {u16{100}, u16{200}, u16{300}}) {
        std::uint64_t region_hash = 0;
        {
            // Open the chunk transaction before releasing the tile producer.
            // The tile worker must reach the same transaction lock before the
            // chunk rewrite proceeds, forcing the formerly racy overlap.
            const RegionFileWriteGuard transaction(path);
            std::latch submit(1);
            bool queued = false;
            std::jthread producer([&] {
                submit.wait();
                queued = writer.enqueue(world.path(), Tile(FarLodTier::F1, height));
            });
            submit.count_down();
            producer.join();
            ASSERT_TRUE(queued);
            WaitForCompetingWriter(transaction);
            chunk->sdf_data.front() = -static_cast<float>(height);
            chunk->mark_voxel_data_dirty();
            ASSERT_TRUE(service.save_world(state, world.path()));
            // A different thread must read the same and unrelated regions and
            // validate the world while this transaction remains held. Joining
            // here also detects an accidentally recursive shared reader lock.
            bool validated = false;
            std::uint64_t observed_unrelated_hash = 0;
            std::jthread reader([&] {
                region_hash = WorldSaveService::region_simulation_digest(world.path(), {-1, 2}, {});
                observed_unrelated_hash =
                    WorldSaveService::region_simulation_digest(world.path(), {1, 0}, {});
                validated = WorldSaveService::validate_save(world.path());
            });
            reader.join();
            EXPECT_TRUE(validated);
            EXPECT_EQ(observed_unrelated_hash, unrelated_hash);
        }
        writer.flush();
        EXPECT_EQ(WorldSaveService::region_simulation_digest(world.path(), {-1, 2}, {}),
                  region_hash);
    }
    writer.close();
    EXPECT_EQ(writer.diagnostics().saved, 3u);
    EXPECT_EQ(writer.diagnostics().failed, 0u);
    WorldStreamingState loaded;
    std::vector<std::string> errors;
    ASSERT_TRUE(service.load_world(loaded, world.path(), errors));
    EXPECT_EQ(service.world_hash(loaded), service.world_hash(state));
    FarLodTile tile;
    ASSERT_TRUE(FarLodStore(world.path()).load_tile(FarLodTier::F1, -1, 2, 1234, tile));
    EXPECT_EQ(tile.height_q.front(), 300u);
    ExpectNoTemps(world.path());
}

TEST(FarLodPersistence, ChunkMergeWaitsForTileTransactionAndPreservesItsPublishedBytes) {
    const SaveDirectory world;
    WorldStreamingState state;
    auto chunk = state.get_or_create_chunk(IVec3(-1, 0, 64));
    chunk->sdf_data = {-1.0f, 2.0f};
    chunk->material_data = {7u, 11u};
    chunk->mark_voxel_data_dirty();
    const WorldSaveService service;
    ASSERT_TRUE(service.save_world(state, world.path()));
    const auto path = WorldSaveService::region_file_path(world.path(), -1, 2);
    const auto simulation_hash = service.world_hash(state);
    const auto region_hash = WorldSaveService::region_simulation_digest(world.path(), {-1, 2}, {});
    std::jthread chunk_writer;
    bool saved = false;
    {
        // Reverse the overlap: hold a tile transaction while the chunk writer
        // validates the world and then reaches the same-file merge lock.
        const RegionFileWriteGuard transaction(path);
        std::latch begin_chunk(1);
        chunk_writer = std::jthread([&] {
            begin_chunk.wait();
            saved = service.save_world(state, world.path());
        });
        begin_chunk.count_down();
        WaitForCompetingWriter(transaction);
        EXPECT_TRUE(FarLodStore(world.path()).save_tile(Tile(FarLodTier::F1, 999)));
        EXPECT_EQ(WorldSaveService::region_simulation_digest(world.path(), {-1, 2}, {}),
                  region_hash);
    }
    chunk_writer.join();
    EXPECT_TRUE(saved);
    FarLodTile tile;
    ASSERT_TRUE(FarLodStore(world.path()).load_tile(FarLodTier::F1, -1, 2, 1234, tile));
    EXPECT_EQ(tile.height_q.front(), 999u);
    WorldStreamingState loaded;
    std::vector<std::string> errors;
    ASSERT_TRUE(service.load_world(loaded, world.path(), errors));
    EXPECT_EQ(service.world_hash(loaded), simulation_hash);
    ExpectNoTemps(world.path());
}

TEST(FarLodPersistence, SaturatedQueueRefusesAt64WithoutMovingAndDrainsInOrder) {
    const SaveDirectory world;
    const SaveDirectory other_world;
    AsyncFarLodStore writer;
    const auto path = WorldSaveService::region_file_path(world.path(), -1, 2);
    FarLodTile refused = Tile(FarLodTier::F2, 999);
    {
        const RegionFileWriteGuard transaction(path);
        ASSERT_TRUE(writer.enqueue(world.path(), Tile(FarLodTier::F1, 100)));
        WaitForCompetingWriter(transaction);
        // One active write is blocked at acquisition. The remaining slots stay
        // queued, so admission and refusal are independent of worker scheduling.
        for (std::size_t index = 1; index < AsyncFarLodStore::kMaxPendingSaves; ++index)
            ASSERT_TRUE(
                writer.enqueue(world.path(), Tile(FarLodTier::F1, static_cast<u16>(100 + index))));
        EXPECT_EQ(writer.diagnostics().pending, 64u);
        EXPECT_EQ(writer.diagnostics().saved, 0u);
        const auto refused_hash = ComputeFarLodTileHash(refused);
        EXPECT_FALSE(TryEnqueue(writer, other_world.path(), refused));
        EXPECT_EQ(ComputeFarLodTileHash(refused), refused_hash);
        EXPECT_EQ(writer.diagnostics().pending, 64u);
        EXPECT_TRUE(std::filesystem::is_empty(other_world.path()));
        ExpectNoTemps(world.path());
    }
    writer.flush();
    EXPECT_EQ(writer.diagnostics().pending, 0u);
    EXPECT_EQ(writer.diagnostics().saved, 64u);
    EXPECT_EQ(writer.diagnostics().failed, 0u);
    FarLodTile loaded;
    ASSERT_TRUE(FarLodStore(world.path()).load_tile(FarLodTier::F1, -1, 2, 1234, loaded));
    EXPECT_EQ(loaded.height_q.front(), 163u);
    ASSERT_TRUE(TryEnqueue(writer, other_world.path(), refused));
    writer.close();
    EXPECT_EQ(writer.diagnostics().saved, 65u);
    ASSERT_TRUE(FarLodStore(other_world.path()).load_tile(FarLodTier::F2, -1, 2, 1234, loaded));
    EXPECT_EQ(loaded.height_q.front(), 999u);
    ExpectNoTemps(world.path());
    ExpectNoTemps(other_world.path());
}

TEST(FarLodIntegration, CountAndByteBudgetsDeferAndEventuallyDrainFifo) {
    struct Upload {
        std::size_t bytes;
        u64 queued_frame;
    };
    std::deque<Upload> queue;
    for (std::size_t index = 0; index < FarLodIntegrationBudget::kMaxBacklog; ++index)
        queue.push_back(Upload{.bytes = index == 17 ? FarLodIntegrationBudget::kMaxBytes + 1 : 1,
                               .queued_frame = 0});
    std::size_t completed = 0;
    bool deferred = false;
    for (u64 frame = 0; frame < 32 && !queue.empty(); ++frame) {
        FarLodIntegrationBudget budget;
        std::size_t frame_completed = 0;
        while (
            !queue.empty() &&
            budget.admit(queue.front().bytes, frame - queue.front().queued_frame, queue.size())) {
            queue.pop_front();
            ++frame_completed;
            ++completed;
        }
        EXPECT_LE(frame_completed, FarLodIntegrationBudget::kMaxUploads);
        deferred = deferred || !queue.empty();
    }
    EXPECT_TRUE(deferred);
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(completed, FarLodIntegrationBudget::kMaxBacklog);
}

TEST(FarLodIntegration, OversizedHeadWaitsForAgeOrBacklogAndConsumesOneFrameAlone) {
    for (u64 age = 0; age < FarLodIntegrationBudget::kMaxDeferredFrames; ++age) {
        FarLodIntegrationBudget budget;
        EXPECT_FALSE(budget.admit(FarLodIntegrationBudget::kMaxBytes + 1, age, 1));
    }
    FarLodIntegrationBudget aged;
    EXPECT_TRUE(aged.admit(
        FarLodIntegrationBudget::kMaxBytes + 1, FarLodIntegrationBudget::kMaxDeferredFrames, 1));
    EXPECT_FALSE(aged.admit(1, FarLodIntegrationBudget::kMaxDeferredFrames, 1));
    FarLodIntegrationBudget pressure;
    EXPECT_TRUE(pressure.admit(
        FarLodIntegrationBudget::kMaxBytes + 1, 0, FarLodIntegrationBudget::kBacklogPressure));
    EXPECT_FALSE(pressure.admit(1, 0, FarLodIntegrationBudget::kBacklogPressure));
    FarLodIntegrationBudget exact;
    EXPECT_TRUE(exact.admit(FarLodIntegrationBudget::kMaxBytes, 0, 1));
    EXPECT_FALSE(exact.admit(1, 0, 1));
}

TEST(FarLodPersistence, CapturesWaitForGenerationPublication) {
    const auto params = Luminumbra::Test::SyntheticTerrainParams();
    Systems::SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    const auto empty = world.capture_far_lod_sdf_snapshot(0, 0);
    ASSERT_TRUE(empty);
    EXPECT_TRUE(empty->entries.empty());
    EXPECT_EQ(empty->bytes_copied, 0u);

    // Inline execution finishes the same jobs without publishing before their
    // due tick. No scheduling delay or thread timing can satisfy this test.
    world.begin_tick(0);
    world.dispatch_generation_jobs(std::vector<Systems::SHIELD_WorldSystem::ChunkGenerationRequest>{
        {IVec3(0, 0, 0), 1}, {IVec3(1, 0, 0), 2}});
    const auto full = world.find_streamed_chunk(IVec3(0, 0, 0));
    const auto coarse = world.find_streamed_chunk(IVec3(1, 0, 0));
    ASSERT_TRUE(full);
    ASSERT_TRUE(coarse);
    ASSERT_EQ(full->get_state(), ChunkState::Loading);
    ASSERT_FALSE(full->sdf_data.empty());
    ASSERT_TRUE(coarse->sdf_data.empty());
    const auto before = world.activation_index_counters();
    world.activate_due(7);
    const auto pending = world.capture_far_lod_sdf_snapshot(0, 0);
    ASSERT_TRUE(pending);
    EXPECT_TRUE(pending->entries.empty());
    EXPECT_EQ(pending->bytes_copied, 0u);
    EXPECT_EQ(world.activation_index_counters().index_updates, before.index_updates);
    EXPECT_TRUE(world.is_far_lod_sdf_snapshot_current(*pending));

    world.activate_due(8);
    ASSERT_EQ(full->get_state(), ChunkState::Idle);
    EXPECT_FALSE(world.is_far_lod_sdf_snapshot_current(*empty));
    EXPECT_FALSE(world.is_far_lod_sdf_snapshot_current(*pending));
    const auto published = world.activation_index_counters();
    EXPECT_GT(published.index_updates, before.index_updates);
    const auto ready = world.capture_far_lod_sdf_snapshot(0, 0);
    ASSERT_TRUE(ready);
    ASSERT_EQ(ready->entries.size(), 1u); // Coarse heightmaps are still excluded.
    ASSERT_TRUE(ready->entries.front().payload);
    EXPECT_EQ(ready->entries.front().coords, full->get_coords());
    EXPECT_EQ(ready->entries.front().as_reduction_snapshot().sdf_data, full->sdf_data);
    EXPECT_EQ(ready->bytes_copied,
              full->sdf_data.size() * sizeof(f32) + full->material_data.size());
    EXPECT_EQ(world.activation_index_counters().index_updates, published.index_updates);
    EXPECT_TRUE(world.is_far_lod_sdf_snapshot_current(*ready));
}

TEST(FarLodPersistence, SnapshotFreshnessCoversNewPublicationsAndPreservesEvictedPayloads) {
    constexpr std::size_t kSamples =
        static_cast<std::size_t>(CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
    constexpr int kSpan = kFarLodRegionSizeMeters / CHUNK_SIZE_X;
    // Both signs, all four halo corners, and all immediately-outside edges.
    for (const int region : {-1, 0}) {
        for (const IVec3 offset : {IVec3(-1, 0, -1),
                                   IVec3(kSpan, 0, -1),
                                   IVec3(-1, 0, kSpan),
                                   IVec3(kSpan, 0, kSpan),
                                   IVec3(-2, 0, 0),
                                   IVec3(kSpan + 1, 0, 0),
                                   IVec3(0, 0, -2),
                                   IVec3(0, 0, kSpan + 1)}) {
            Systems::SHIELD_WorldSystem world(
                nullptr, nullptr, Luminumbra::Test::SyntheticTerrainParams(), 1337);
            const auto before = world.capture_far_lod_sdf_snapshot(region, region);
            ASSERT_TRUE(before);
            ASSERT_TRUE(before->entries.empty());
            auto chunk = std::make_shared<Chunk>(offset + IVec3(region * kSpan, 0, region * kSpan));
            ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
            // Missing/partial SDF and material lattices are still ineligible.
            EXPECT_TRUE(world.is_far_lod_sdf_snapshot_current(*before));
            chunk->sdf_data.assign(kSamples - 1, -1.0f);
            EXPECT_TRUE(world.is_far_lod_sdf_snapshot_current(*before));
            chunk->sdf_data.push_back(-1.0f);
            chunk->material_data.assign(kSamples - 1, 7u);
            EXPECT_TRUE(world.is_far_lod_sdf_snapshot_current(*before));
            chunk->material_data.push_back(7u);
            const bool inside =
                offset.x >= -1 && offset.x <= kSpan && offset.z >= -1 && offset.z <= kSpan;
            EXPECT_EQ(world.is_far_lod_sdf_snapshot_current(*before), !inside);
            const auto after = world.capture_far_lod_sdf_snapshot(region, region);
            ASSERT_TRUE(after);
            EXPECT_EQ(after->entries.size(), inside ? 1u : 0u);
            EXPECT_TRUE(world.is_far_lod_sdf_snapshot_current(*after));
            if (inside) {
                chunk->set_state(ChunkState::Loading);
                EXPECT_FALSE(world.is_far_lod_sdf_snapshot_current(*after));
                chunk->set_state(ChunkState::Idle);
                chunk->sdf_data.pop_back();
                EXPECT_FALSE(world.is_far_lod_sdf_snapshot_current(*after));
                chunk->sdf_data.push_back(-1.0f);
                chunk->material_data.pop_back();
                EXPECT_FALSE(world.is_far_lod_sdf_snapshot_current(*after));
                chunk->material_data.push_back(7u);
                EXPECT_TRUE(world.is_far_lod_sdf_snapshot_current(*after));
            }
            // Eviction preserves captured authority without requiring live storage.
            world.clear_world(nullptr);
            EXPECT_TRUE(world.is_far_lod_sdf_snapshot_current(*after));
            if (inside)
                EXPECT_EQ(after->entries.front().payload->sdf_data.size(), kSamples);

            auto invalid = *after;
            for (const int bad :
                 {std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
                invalid.min_chunk_coords = IVec2(bad, 0);
                EXPECT_FALSE(world.is_far_lod_sdf_snapshot_current(invalid));
                invalid.min_chunk_coords = IVec2(0, bad);
                EXPECT_FALSE(world.is_far_lod_sdf_snapshot_current(invalid));
            }
        }
    }
}

TEST(FarLodPersistence, UnchangedAndOverlappingCapturesShareBytesAndEditsCopyOnce) {
    const auto params = Luminumbra::Test::SyntheticTerrainParams();
    Systems::SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    auto chunk = std::make_shared<Chunk>(IVec3(31, 0, 0)); // shared region halo
    constexpr std::size_t kSamples =
        static_cast<std::size_t>(CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
    chunk->sdf_data.assign(kSamples, -1.0f);
    chunk->material_data.assign(kSamples, 7u);
    ASSERT_TRUE(world.adopt_streamed_chunk(chunk));
    const auto first = world.capture_far_lod_sdf_snapshot(0, 0);
    const auto again = world.capture_far_lod_sdf_snapshot(0, 0);
    const auto neighbour = world.capture_far_lod_sdf_snapshot(1, 0);
    ASSERT_EQ(first->entries.size(), 1u);
    ASSERT_EQ(again->entries.size(), 1u);
    ASSERT_EQ(neighbour->entries.size(), 1u);
    EXPECT_EQ(first->bytes_copied, kSamples * (sizeof(f32) + sizeof(u8)));
    EXPECT_EQ(again->bytes_copied, 0u);
    EXPECT_EQ(neighbour->bytes_copied, 0u);
    EXPECT_EQ(first->entries.front().payload, again->entries.front().payload);
    EXPECT_EQ(first->entries.front().payload, neighbour->entries.front().payload);
    chunk->material_data.front() = 9u; // even an unmarked replacement must copy
    const auto changed = world.capture_far_lod_sdf_snapshot(0, 0);
    ASSERT_EQ(changed->entries.size(), 1u);
    EXPECT_EQ(changed->bytes_copied, first->bytes_copied);
    EXPECT_NE(changed->entries.front().payload, first->entries.front().payload);
    EXPECT_EQ(first->entries.front().payload->material_data.front(), 7u);
    EXPECT_EQ(changed->entries.front().payload->material_data.front(), 9u);
    EXPECT_EQ(world.capture_far_lod_sdf_snapshot(0, 0)->bytes_copied, 0u);
    chunk->sdf_data.front() = 3.0f;
    chunk->mark_voxel_data_dirty();
    const auto edited = world.capture_far_lod_sdf_snapshot(0, 0);
    EXPECT_EQ(edited->bytes_copied, first->bytes_copied);
    EXPECT_EQ(edited->entries.front().payload->sdf_data.front(), 3.0f);
    EXPECT_EQ(first->entries.front().payload->sdf_data.front(), -1.0f);
    EXPECT_FALSE(world.is_far_lod_sdf_snapshot_current(*first));
    EXPECT_TRUE(world.is_far_lod_sdf_snapshot_current(*edited));
    EXPECT_FALSE(edited->entries.front().authority_durable);
    chunk->clear_voxel_data_dirty();
    const auto durable = world.capture_far_lod_sdf_snapshot(0, 0);
    EXPECT_EQ(durable->bytes_copied, 0u);
    EXPECT_TRUE(durable->entries.front().authority_durable);
    EXPECT_EQ(durable->entries.front().payload, edited->entries.front().payload);
}

namespace {
// Exercise the production owner-thread integration and world-swap paths with
// recorded GL entry points. No context, display, driver, clock or test skip.
template<typename T>
class ScopedGlEntry {
public:
    ScopedGlEntry(T& entry, T replacement)
        : m_entry(entry)
        , m_previous(entry) {
        m_entry = replacement;
    }
    ~ScopedGlEntry() {
        m_entry = m_previous;
    }

private:
    T& m_entry;
    T m_previous;
};

void APIENTRY GenerateObjects(GLsizei count, GLuint* names) {
    static GLuint next_name = 1;
    for (GLsizei index = 0; index < count; ++index)
        names[index] = next_name++;
}
void APIENTRY DeleteObjects(GLsizei, const GLuint*) {}
void APIENTRY BindVertexArray(GLuint) {}
void APIENTRY BindBuffer(GLenum, GLuint) {}
void APIENTRY UploadBuffer(GLenum, GLsizeiptr, const void*, GLenum) {}
void APIENTRY EnableAttribute(GLuint) {}
void APIENTRY FloatAttribute(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) {}
void APIENTRY IntegerAttribute(GLuint, GLint, GLenum, GLsizei, const void*) {}
GLboolean APIENTRY IsObject(GLuint) {
    return GL_TRUE;
}

// Recorded GL entries shared by owner-thread integration regressions.
struct ScopedFarGl {
    const ScopedGlEntry<decltype(glGenBuffers)> gen_buffers{glGenBuffers, &GenerateObjects};
    const ScopedGlEntry<decltype(glGenVertexArrays)> gen_arrays{glGenVertexArrays,
                                                                &GenerateObjects};
    const ScopedGlEntry<decltype(glDeleteBuffers)> delete_buffers{glDeleteBuffers, &DeleteObjects};
    const ScopedGlEntry<decltype(glDeleteVertexArrays)> delete_arrays{glDeleteVertexArrays,
                                                                      &DeleteObjects};
    const ScopedGlEntry<decltype(glBindVertexArray)> bind_array{glBindVertexArray,
                                                                &BindVertexArray};
    const ScopedGlEntry<decltype(glBindBuffer)> bind_buffer{glBindBuffer, &BindBuffer};
    const ScopedGlEntry<decltype(glBufferData)> upload{glBufferData, &UploadBuffer};
    const ScopedGlEntry<decltype(glEnableVertexAttribArray)> enable{glEnableVertexAttribArray,
                                                                    &EnableAttribute};
    const ScopedGlEntry<decltype(glVertexAttribPointer)> float_attribute{glVertexAttribPointer,
                                                                         &FloatAttribute};
    const ScopedGlEntry<decltype(glVertexAttribIPointer)> integer_attribute{glVertexAttribIPointer,
                                                                            &IntegerAttribute};
    const ScopedGlEntry<decltype(glIsBuffer)> is_buffer{glIsBuffer, &IsObject};
    const ScopedGlEntry<decltype(glIsVertexArray)> is_array{glIsVertexArray, &IsObject};
    const ScopedGlEntry<decltype(glObjectLabel)> label{
        glObjectLabel, static_cast<decltype(glObjectLabel)>(nullptr)};
};
} // namespace

TEST(FarLodPersistence, PublicationAfterCaptureRefusesTilePersistence) {
    const ScopedFarGl gl;
    const SaveDirectory save;
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.base_amplitude = 0.0f;
    params.height_offset = 12.0f;
    params.caves_enabled = false;
    Systems::SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    world.begin_tick(0);
    world.dispatch_generation_jobs(
        std::vector<Systems::SHIELD_WorldSystem::ChunkGenerationRequest>{{IVec3(0, 0, 0), 1}});
    const auto chunk = world.find_streamed_chunk(IVec3(0, 0, 0));
    ASSERT_TRUE(chunk);
    ASSERT_EQ(chunk->get_state(), ChunkState::Loading);
    JobSystem jobs;
    jobs.startup(1);
    Rendering::FarLodSystem far;
    far.attach_job_system(&jobs);
    far.set_save_dir(save.path());
    const Vec3 camera(256, 100, 256);
    far.update(world, camera);
    ASSERT_GT(far.stats().builds_dispatched, 0u);
    // Complete builds without accepting them, then publish the skipped chunk.
    far.wait_for_world_readers();
    world.activate_due(8);
    ASSERT_EQ(chunk->get_state(), ChunkState::Idle);
    // A local edit after publication is authoritative, but this chunk was
    // absent from the capture. Neither captured-entry revisions nor its
    // captured durability predicate can detect these newly contributing bytes.
    const auto authority_revision = world.far_lod_authority_revision();
    ASSERT_FALSE(chunk->sdf_data.empty());
    chunk->sdf_data.front() += 1.0f;
    chunk->mark_voxel_data_dirty();
    EXPECT_EQ(world.far_lod_authority_revision(), authority_revision);
    far.update(world, camera);
    EXPECT_GT(far.stats().stale_results_rejected, 0u);
    // Replacement builds have not been accepted; discard them and flush writes.
    far.prepare_world_swap();
    FarLodTile loaded;
    EXPECT_FALSE(
        FarLodStore(save.path())
            .load_tile(FarLodTier::F1, 0, 0, ComputeTerrainParamsHash(params, 1337), loaded));
    ExpectNoTemps(save.path());
    far.shutdown();
    jobs.shutdown();
}

TEST(FarLodPersistence, SaveFenceCompletesAcceptedWritesAndRetainsBindingAndDeferredUploads) {
    const ScopedFarGl gl;
    const SaveDirectory save;
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.base_amplitude = 0.0f;
    params.height_offset = 12.0f;
    params.caves_enabled = false;
    const Systems::SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    JobSystem jobs;
    jobs.startup(1);
    Rendering::FarLodSystem far;
    far.attach_job_system(&jobs);
    far.set_save_dir(save.path());
    far.wait_for_accepted_cache_writes(); // empty writer queue
    far.wait_for_world_readers();         // no readers yet
    const Vec3 camera(256, 100, 256);
    far.update(world, camera);
    jobs.wait(jobs.dispatch_batch({[] {
    }}));
    far.update(world, camera);
    ASSERT_GT(far.stats().integration_uploads_deferred, 0u);
    const auto resident = far.stats().regions_resident;
    far.wait_for_accepted_cache_writes();
    // The fence returns only after every accepted tile has reached its captured path.
    // Dispatch admits at most kMaxBuildDispatchesPerFrame writes in this first batch.
    std::size_t regions = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(save.path()))
        if (entry.path().extension() == ".lmr")
            ++regions;
    EXPECT_EQ(regions, Rendering::FarLodSystem::kMaxBuildDispatchesPerFrame);
    FarLodTile loaded;
    ASSERT_TRUE(
        FarLodStore(save.path())
            .load_tile(FarLodTier::F1, 0, 0, ComputeTerrainParamsHash(params, 1337), loaded));
    EXPECT_FALSE(loaded.height_q.empty());
    ExpectNoTemps(save.path());
    EXPECT_EQ(far.stats().regions_resident, resident);
    far.wait_for_world_readers(); // reader completion also retains unpublished results
    // Resuming the identical world must integrate retained deferred work. An
    // unbound world instead discards it and dispatches a fresh first batch.
    far.update(world, camera);
    EXPECT_GT(far.stats().builds_integrated_ok, 0u);
    EXPECT_EQ(far.stats().tile_save_failures, 0u);
    far.shutdown();
    jobs.shutdown();
}

TEST(FarLodPersistence, SaveFenceFinishesFailedWritesWithoutDiscardingDeferredUploads) {
    const ScopedFarGl gl;
    const SaveDirectory save;
    const auto blocked = save.path() / "blocked";
    ASSERT_TRUE(std::filesystem::create_directory(blocked));
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.base_amplitude = 0.0f;
    params.height_offset = 12.0f;
    params.caves_enabled = false;
    const Systems::SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    JobSystem jobs;
    jobs.startup(1);
    Rendering::FarLodSystem far;
    far.attach_job_system(&jobs);
    far.set_save_dir(blocked);
    const Vec3 camera(256, 100, 256);
    far.update(world, camera);
    jobs.wait(jobs.dispatch_batch({[] {
    }}));
    // Finish successful reads before making the captured destination unwritable.
    // The completed batch is accepted for persistence only by the following update.
    ASSERT_TRUE(std::filesystem::remove(blocked));
    {
        std::ofstream output(blocked);
        output << "regular file prevents cache directory creation";
        ASSERT_TRUE(output.good());
    }
    far.update(world, camera);
    ASSERT_GT(far.stats().integration_uploads_deferred, 0u);
    far.wait_for_accepted_cache_writes();
    far.update(world, camera);
    EXPECT_GT(far.stats().builds_integrated_ok, 0u);
    EXPECT_GE(far.stats().tile_save_failures, Rendering::FarLodSystem::kMaxBuildDispatchesPerFrame);
    EXPECT_TRUE(std::filesystem::is_regular_file(blocked));
    far.shutdown();
    jobs.shutdown();
}

TEST(FarLodPersistence, ProductionWorldSwapFlushesSavesEvenWhenUploadsAreDeferred) {
    const ScopedFarGl gl;

    const SaveDirectory old_world;
    const SaveDirectory new_world;
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.base_amplitude = 0.0f;
    params.height_offset = 12.0f;
    params.caves_enabled = false;
    const Systems::SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    JobSystem jobs;
    jobs.startup(1);
    Rendering::FarLodSystem far;
    far.attach_job_system(&jobs);
    far.set_save_dir(old_world.path());
    const Vec3 camera(256, 100, 256);
    far.update(world, camera);
    ASSERT_EQ(far.stats().builds_dispatched, Rendering::FarLodSystem::kMaxBuildDispatchesPerFrame);
    // One FIFO worker and a following fence complete precisely the first batch.
    jobs.wait(jobs.dispatch_batch({[] {
    }}));
    far.update(world, camera);
    EXPECT_LE(far.stats().builds_integrated_ok, FarLodIntegrationBudget::kMaxUploads);
    EXPECT_GT(far.stats().integration_uploads_deferred, 0u);
    EXPECT_EQ(far.stats().builds_integrated_ok + far.stats().integration_uploads_deferred,
              Rendering::FarLodSystem::kMaxBuildDispatchesPerFrame);
    // Cancels builds not accepted for persistence and flushes the eight accepted
    // saves before changing the path. Deferred GL work cannot hold saves back.
    far.set_save_dir(new_world.path());
    far.set_enabled(false);
    far.update(world, camera);
    EXPECT_EQ(far.stats().tiles_saved_async, Rendering::FarLodSystem::kMaxBuildDispatchesPerFrame);
    EXPECT_EQ(far.stats().pending_saves, 0u);
    EXPECT_EQ(far.stats().tile_save_failures, 0u);
    std::size_t regions = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(old_world.path()))
        if (entry.path().extension() == ".lmr")
            ++regions;
    EXPECT_EQ(regions, Rendering::FarLodSystem::kMaxBuildDispatchesPerFrame);
    EXPECT_TRUE(std::filesystem::is_empty(new_world.path()));
    ExpectNoTemps(old_world.path());
    ExpectNoTemps(new_world.path());
    far.shutdown();
    jobs.shutdown();
}

TEST(FarLodPersistence, SaveBackpressureBoundsBuildBacklogAndResumesAfterDrain) {
    const ScopedFarGl gl;
    const SaveDirectory world_save;
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.base_amplitude = 0.0f;
    params.height_offset = 12.0f;
    params.caves_enabled = false;
    const Systems::SHIELD_WorldSystem world(nullptr, nullptr, params, 1337);
    JobSystem jobs;
    jobs.startup(1);
    Rendering::FarLodSystem far;
    far.attach_job_system(&jobs);
    far.set_save_dir(world_save.path());
    const Vec3 camera(256, 100, 256);
    std::size_t dispatched = 0;
    const auto path = WorldSaveService::region_file_path(world_save.path(), 0, 0);
    {
        // This is the unique nearest tile to the camera, hence the first save
        // on the single FIFO worker. Keep it blocked until both queues fill.
        const RegionFileWriteGuard transaction(path);
        far.update(world, camera);
        dispatched += far.stats().builds_dispatched;
        jobs.wait(jobs.dispatch_batch({[] {
        }}));
        far.update(world, camera);
        dispatched += far.stats().builds_dispatched;
        WaitForCompetingWriter(transaction);
        // Every fence finishes the preceding build batch. Persistence cannot
        // advance, so 24 frames suffice to reach both unchanged capacity limits.
        for (std::size_t frame = 0; frame < 24; ++frame) {
            jobs.wait(jobs.dispatch_batch({[] {
            }}));
            far.update(world, camera);
            dispatched += far.stats().builds_dispatched;
            EXPECT_LE(far.stats().pending_saves, AsyncFarLodStore::kMaxPendingSaves);
            EXPECT_LE(far.stats().pending_depth, FarLodIntegrationBudget::kMaxBacklog);
            EXPECT_LE(far.stats().builds_integrated_ok, FarLodIntegrationBudget::kMaxUploads);
        }
        EXPECT_EQ(far.stats().pending_saves, AsyncFarLodStore::kMaxPendingSaves);
        EXPECT_EQ(far.stats().pending_depth, FarLodIntegrationBudget::kMaxBacklog);
        EXPECT_EQ(far.stats().builds_dispatched, 0u);
        EXPECT_EQ(dispatched,
                  AsyncFarLodStore::kMaxPendingSaves + FarLodIntegrationBudget::kMaxBacklog);
    }
    // World-boundary flush is a deterministic worker fence. It cancels the
    // unaccepted build backlog and preserves every previously accepted save.
    far.prepare_world_swap();
    far.set_enabled(false);
    far.update(world, camera);
    EXPECT_EQ(far.stats().pending_saves, 0u);
    EXPECT_EQ(far.stats().tiles_saved_async, AsyncFarLodStore::kMaxPendingSaves);
    EXPECT_EQ(far.stats().tile_save_failures, 0u);
    EXPECT_EQ(far.stats().pending_depth, 0u);
    ExpectNoTemps(world_save.path());
    far.set_enabled(true);
    far.update(world, camera);
    EXPECT_GT(far.stats().builds_dispatched, 0u);
    far.shutdown();
    jobs.shutdown();
}
