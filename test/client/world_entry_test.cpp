#include "../support/PersistenceFileOps.h"
#include "../support/WorldgenTestData.h"
#include "luminumbra_client/world/WorldEntry.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/fields/EnergyFieldState.h"
#include "luminumbra_common/persistence/CreatedWorldArtifacts.h"
#include "luminumbra_common/persistence/SyntheticSaveFixture.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/ActiveRegionLedger.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/world/WorldClock.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using Luminumbra::Client::WorldEntry;
using Luminumbra::Client::WorldEntryResult;
using Luminumbra::Client::WorldEntryStage;
using Luminumbra::Client::WorldEntryState;
using Luminumbra::Persistence::CreatedWorldArtifacts;

constexpr auto kStageCount = static_cast<std::size_t>(WorldEntryStage::Count);
struct Request {
    std::string identity;
    std::shared_ptr<int> lifetime;
};

struct Runner {
    std::array<unsigned, kStageCount> pending{};
    std::optional<WorldEntryStage> refusal;
    std::vector<WorldEntryStage> calls;
    std::vector<WorldEntryStage> authoritative_steps;
    std::string opened_identity;
    std::string error;
    unsigned drains = 0;
    unsigned pending_drains = 0;

    WorldEntryResult run(WorldEntryStage stage, const Request& request) {
        calls.push_back(stage);
        auto& remaining = pending[static_cast<std::size_t>(stage)];
        if (remaining != 0) {
            --remaining;
            return WorldEntryResult::Pending;
        }
        if (refusal == stage) {
            error = "World open refused";
            return WorldEntryResult::Failed;
        }
        opened_identity = request.identity;
        authoritative_steps.push_back(stage);
        return WorldEntryResult::Complete;
    }
    WorldEntryResult drain() {
        ++drains;
        if (pending_drains != 0) {
            --pending_drains;
            return WorldEntryResult::Pending;
        }
        return WorldEntryResult::Complete;
    }
};

void Advance(WorldEntry<Request>& entry, Runner& runner) {
    entry.advance(
        [&](WorldEntryStage stage, const Request& request) { return runner.run(stage, request); },
        [&] { return runner.drain(); });
}

TEST(WorldEntry, CallbackOnlyOwnsRequestUntilLoadingHasBeenPresented) {
    WorldEntry<Request> entry;
    Runner runner;
    entry.loading_painted(); // A previous menu presentation cannot satisfy a new request.
    Request form{.identity = "saved-world", .lifetime = {}};
    ASSERT_TRUE(entry.request(form));
    form.identity = "edited after click";
    EXPECT_EQ(entry.state(), WorldEntryState::Requested);
    EXPECT_TRUE(runner.calls.empty());
    EXPECT_FALSE(entry.request(Request{.identity = "double click", .lifetime = {}}));
    for (unsigned frame = 0; frame < 3; ++frame)
        Advance(entry, runner);
    EXPECT_TRUE(runner.calls.empty());
    EXPECT_FLOAT_EQ(entry.progress(), 0.0f);

    entry.loading_painted(); // Host has submitted AND presented the loading frame.
    EXPECT_EQ(entry.state(), WorldEntryState::LoadingPainted);
    EXPECT_TRUE(runner.calls.empty());
    Advance(entry, runner);
    ASSERT_EQ(runner.calls.size(), 1u);
    EXPECT_EQ(runner.calls.front(), WorldEntryStage::Validate);
    EXPECT_EQ(runner.opened_identity, "saved-world");
    EXPECT_GT(entry.progress(), 0.0f);
}

TEST(WorldEntry, OneStagePerAdvanceAndPendingNeverChangesAuthoritativeOrder) {
    std::vector<WorldEntryStage> expected;
    for (std::size_t index = 0; index < kStageCount; ++index)
        expected.push_back(static_cast<WorldEntryStage>(index));
    for (const unsigned delay : {0u, 1u, 7u}) {
        WorldEntry<Request> entry;
        Runner runner;
        runner.pending.fill(delay);
        ASSERT_TRUE(entry.request(Request{.identity = "fixed-seed-and-save", .lifetime = {}}));
        entry.loading_painted();
        for (std::size_t frame = 0; frame < kStageCount * (delay + 1); ++frame) {
            const auto before = runner.calls.size();
            const auto stage = entry.stage();
            Advance(entry, runner);
            ASSERT_EQ(runner.calls.size(), before + 1);
            EXPECT_EQ(runner.calls.back(), stage);
        }
        EXPECT_EQ(entry.state(), WorldEntryState::Ready);
        EXPECT_FALSE(entry.active());
        EXPECT_FLOAT_EQ(entry.progress(), 1.0f);
        EXPECT_EQ(runner.authoritative_steps, expected);
        EXPECT_EQ(runner.opened_identity, "fixed-seed-and-save");
        Advance(entry, runner);
        EXPECT_EQ(runner.calls.size(), kStageCount * (delay + 1));
        EXPECT_EQ(runner.drains, 0u);
    }
}

TEST(WorldEntry, RefusalStopsAllLaterStagesAndAllowsRetry) {
    WorldEntry<Request> entry;
    Runner runner;
    runner.refusal = WorldEntryStage::OpenWorld;
    ASSERT_TRUE(entry.request(Request{.identity = "incompatible-save", .lifetime = {}}));
    entry.loading_painted();
    Advance(entry, runner);
    Advance(entry, runner);
    Advance(entry, runner);
    EXPECT_EQ(entry.state(), WorldEntryState::Failed);
    EXPECT_FALSE(entry.active());
    EXPECT_EQ(runner.error, "World open refused");
    EXPECT_EQ(runner.calls.back(), WorldEntryStage::OpenWorld);
    const auto calls = runner.calls.size();
    Advance(entry, runner);
    EXPECT_EQ(runner.calls.size(), calls);
    runner.refusal.reset();
    ASSERT_TRUE(entry.request(Request{.identity = "repaired-save", .lifetime = {}}));
    EXPECT_EQ(entry.stage(), WorldEntryStage::Validate);
    Advance(entry, runner);
    EXPECT_EQ(runner.calls.size(), calls); // Retry needs its own loading presentation.
}

TEST(WorldEntry, CancellationBeforePaintRunsOnlyOwnershipDrain) {
    WorldEntry<Request> entry;
    Runner runner;
    ASSERT_TRUE(entry.request(Request{.identity = "cancelled", .lifetime = {}}));
    entry.cancel();
    entry.loading_painted();
    Advance(entry, runner);
    EXPECT_EQ(entry.state(), WorldEntryState::Cancelled);
    EXPECT_TRUE(runner.calls.empty());
    EXPECT_EQ(runner.drains, 1u);
    Advance(entry, runner);
    EXPECT_EQ(runner.drains, 1u);
}

TEST(WorldEntry, CancelAtEveryStageRetainsOwnersUntilDrainCompletes) {
    for (std::size_t stage_index = 0; stage_index < kStageCount; ++stage_index) {
        WorldEntry<Request> entry;
        Runner runner;
        auto owner = std::make_shared<int>(1);
        const std::weak_ptr<int> lifetime = owner;
        ASSERT_TRUE(entry.request(Request{.identity = "world", .lifetime = std::move(owner)}));
        entry.loading_painted();
        for (std::size_t index = 0; index < stage_index; ++index)
            Advance(entry, runner);
        runner.pending[stage_index] = 1;
        Advance(entry, runner); // A worker or GL stage is still in flight.
        const auto calls = runner.calls.size();
        runner.pending_drains = 1;
        entry.cancel();
        Advance(entry, runner);
        EXPECT_EQ(entry.state(), WorldEntryState::Cancelling);
        EXPECT_FALSE(lifetime.expired());
        EXPECT_FALSE(entry.request(Request{.identity = "replacement", .lifetime = {}}));
        entry.loading_painted();
        Advance(entry, runner);
        EXPECT_EQ(entry.state(), WorldEntryState::Cancelled);
        EXPECT_TRUE(lifetime.expired());
        EXPECT_EQ(runner.calls.size(), calls);
        EXPECT_EQ(runner.drains, 2u);
    }
}

TEST(WorldEntry, PressAndReleaseBetweenAdvancesCancelsBeforeEveryNextStage) {
    for (std::size_t boundary = 0; boundary < kStageCount; ++boundary) {
        WorldEntry<Request> entry;
        Runner runner;
        ASSERT_TRUE(entry.request(Request{.identity = "world", .lifetime = {}}));
        entry.loading_painted();
        for (std::size_t index = 0; index < boundary; ++index)
            Advance(entry, runner);
        const auto calls = runner.calls.size();
        // GLFW can deliver both events in one poll after a blocking stage.
        entry.cancel_input(true);
        entry.cancel_input(false);
        Advance(entry, runner);
        EXPECT_EQ(entry.state(), WorldEntryState::Cancelled) << boundary;
        EXPECT_EQ(runner.calls.size(), calls) << boundary;
        EXPECT_EQ(runner.drains, 1u) << boundary;
    }
}

TEST(WorldEntry, ReleaseAloneAndInputOutsideAnEntryDoNotCancelTheNextRequest) {
    WorldEntry<Request> entry;
    Runner runner;
    entry.cancel_input(true);
    ASSERT_TRUE(entry.request(Request{.identity = "world", .lifetime = {}}));
    entry.loading_painted();
    entry.cancel_input(false);
    Advance(entry, runner);
    EXPECT_EQ(entry.state(), WorldEntryState::Running);
    ASSERT_EQ(runner.calls.size(), 1u);
    EXPECT_EQ(runner.calls.front(), WorldEntryStage::Validate);
    EXPECT_EQ(runner.drains, 0u);
}

class WorldEntryFilesystem : public testing::Test {
protected:
    void SetUp() override {
        std::random_device entropy;
        for (;;) {
            const auto candidate =
                std::filesystem::temp_directory_path() /
                ("world-entry-" + std::to_string(entropy()) + "-" + std::to_string(entropy()));
            if (std::filesystem::create_directory(candidate)) {
                root = candidate;
                break;
            }
        }
        Luminumbra::TestSupport::CopyWorldgenTestData(LUMINUMBRA_SOURCE_ROOT, root);
    }
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        EXPECT_FALSE(error) << error.message();
    }
    static void Write(const std::filesystem::path& path, const std::string& bytes) {
        std::ofstream output(path, std::ios::binary);
        output << bytes;
        output.close(); // Windows rollback must never encounter an open fixture file.
        ASSERT_TRUE(output.good());
    }
    static std::string Read(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }
    void InstallPresets() {
        const auto presets = root / "worlds/atlas/presets";
        std::filesystem::create_directories(presets);
        std::filesystem::copy_file(std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) /
                                       "worlds/atlas/presets/default.json",
                                   presets / "default.json");
        std::filesystem::copy(std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) /
                                  "worlds/atlas/presets-rev6",
                              root / "worlds/atlas/presets-rev6",
                              std::filesystem::copy_options::recursive);
    }
    void Publish(CreatedWorldArtifacts& artifacts, const std::filesystem::path& save) {
        artifacts.create_root(save);
        artifacts.write_file(save / "preset.json", "preset bytes\n");
        artifacts.write_file(save / "world_info.json", "metadata bytes\n");
        artifacts.reserve_file(save / "world_info.json.tmp.fixture");
        artifacts.create_directory(save / "chunks");
        artifacts.create_directory(save / "chunks/region");
        artifacts.write_file(save / "chunks/region/active-regions.arl", "ledger bytes\n");
        // A failed temporary-file cleanup must still be owned by rollback.
        artifacts.write_file(save / "active-regions.arl.tmp.fixture", "partial ledger bytes\n");
    }
    static std::map<std::string, std::string> DiskBytes(const std::filesystem::path& directory) {
        std::map<std::string, std::string> result;
        for (const auto& item : std::filesystem::recursive_directory_iterator(directory))
            result.emplace(Luminumbra::Filesystem::GenericUtf8PathString(
                               item.path().lexically_relative(directory)),
                           item.is_directory() ? "<directory>" : Read(item.path()));
        return result;
    }
    std::filesystem::path root;
};

TEST_F(WorldEntryFilesystem, PreparationNeedsNoLiveSessionAndCancellationNeverAdopts) {
    using Luminumbra::world::GameSession;
    InstallPresets();
    GameSession::WorldPreparationOptions options;
    options.root_path = Luminumbra::Filesystem::GenericUtf8PathString(root) + "/";
    options.save_root = root;
    options.configured_save_root = root;
    options.job_system_available = true;
    const auto before = DiskBytes(root);
    std::stop_source stop;
    std::shared_ptr<GameSession::WorldPreparation> creation;
    std::jthread worker([&] {
        creation = GameSession::PrepareWorldCreation(options,
                                                     "Prepared",
                                                     "1337",
                                                     "default",
                                                     nullptr,
                                                     Luminumbra::world::TerrainPresetUse::NewWorld,
                                                     {},
                                                     stop.get_token());
    });
    worker.join();
    ASSERT_TRUE(creation);
    EXPECT_TRUE(GameSession::WorldPreparationError(*creation).empty());
    EXPECT_EQ(DiskBytes(root), before); // Preparing creation writes nothing.
    stop.request_stop();                // Also refuse cancellation after worker completion.
    const auto abandoned_load = GameSession::PrepareWorldLoad(options, "missing", stop.get_token());
    EXPECT_EQ(GameSession::WorldPreparationError(*abandoned_load),
              "Saved-world validation cancelled.");
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    {
        GameSession session;
        session.SetRootPath(options.root_path);
        session.SetJobSystem(&jobs);
        const auto revision = session.WorldRevision();
        EXPECT_FALSE(session.AdoptPreparedWorld(creation));
        EXPECT_EQ(session.GetWorldOpenError(), "Saved-world validation cancelled.");
        EXPECT_EQ(session.WorldRevision(), revision);
        EXPECT_EQ(session.GetWorldSystem(), nullptr);
        EXPECT_EQ(DiskBytes(root), before);

        ASSERT_TRUE(session.CreateWorld("Retained", "1337", "default"));
        const auto id = session.GetMetadata().worldId;
        auto* world = session.GetWorldSystem();
        world->SetWaterSimWindowCursor(19);
        const auto live_revision = session.WorldRevision();
        const auto saved_bytes = DiskBytes(root);
        for (const bool create : {false, true}) {
            auto cancelled = create ? GameSession::PrepareWorldCreation(
                                          options,
                                          "Cancelled",
                                          "0",
                                          "default",
                                          nullptr,
                                          Luminumbra::world::TerrainPresetUse::NewWorld,
                                          {},
                                          stop.get_token())
                                    : GameSession::PrepareWorldLoad(options, id, stop.get_token());
            EXPECT_FALSE(session.AdoptPreparedWorld(cancelled));
            EXPECT_EQ(session.GetWorldSystem(), world);
            EXPECT_EQ(session.WorldRevision(), live_revision);
            EXPECT_EQ(world->GetWaterSimWindowCursor(), 19u);
            EXPECT_EQ(DiskBytes(root), saved_bytes);
        }
        std::stop_source late_cancel;
        auto loaded = GameSession::PrepareWorldLoad(options, id, late_cancel.get_token());
        ASSERT_TRUE(GameSession::WorldPreparationError(*loaded).empty());
        late_cancel.request_stop();
        EXPECT_FALSE(session.AdoptPreparedWorld(loaded));
        EXPECT_EQ(session.GetWorldSystem(), world);
        EXPECT_EQ(session.WorldRevision(), live_revision);
        EXPECT_EQ(world->GetWaterSimWindowCursor(), 19u);
        EXPECT_EQ(DiskBytes(root), saved_bytes);

        auto state_options = session.CaptureWorldPreparationOptions();
        auto initial_state =
            GameSession::PrepareWorldState(state_options, session.GetWorldSaveDir());
        ASSERT_TRUE(GameSession::WorldPreparationError(*initial_state).empty());
        ASSERT_TRUE(session.AdoptPreparedWorld(initial_state));
        EXPECT_EQ(session.GetWorldSystem(), world);
        EXPECT_EQ(session.WorldRevision(), live_revision);
        auto stale_state = GameSession::PrepareWorldState(state_options, session.GetWorldSaveDir());
        auto valid = GameSession::PrepareWorldLoad(state_options, id);
        ASSERT_TRUE(session.AdoptPreparedWorld(valid)) << session.GetWorldOpenError();
        const auto adopted_revision = session.WorldRevision();
        EXPECT_FALSE(session.AdoptPreparedWorld(valid));
        EXPECT_EQ(session.WorldRevision(), adopted_revision);
        EXPECT_FALSE(session.AdoptPreparedWorld({}));
        EXPECT_FALSE(session.AdoptPreparedWorld(stale_state));
        EXPECT_EQ(session.GetWorldOpenError(),
                  "World preparation no longer matches session configuration.");
        EXPECT_EQ(session.WorldRevision(), adopted_revision);
    }
    jobs.shutdown();
}

struct CancelPreparationRead {
    std::stop_source stop;
    std::size_t opens = 0;
    std::size_t cancel_at = 0;
    static CancelPreparationRead*& Active() {
        static CancelPreparationRead* active = nullptr;
        return active;
    }
    static void Read(const std::filesystem::path&, bool opening) {
        auto& state = *Active();
        if (opening && ++state.opens == state.cancel_at)
            state.stop.request_stop();
    }
    explicit CancelPreparationRead(std::size_t ordinal)
        : cancel_at(ordinal) {
        Active() = this;
        Luminumbra::Persistence::detail::set_file_read_observer_for_testing(&Read);
    }
    ~CancelPreparationRead() {
        Luminumbra::Persistence::detail::set_file_read_observer_for_testing(nullptr);
        Active() = nullptr;
    }
    CancelPreparationRead(const CancelPreparationRead&) = delete;
    CancelPreparationRead& operator=(const CancelPreparationRead&) = delete;
};

TEST_F(WorldEntryFilesystem, CancellationAtEverySavedFileReadPreservesLiveState) {
    using Luminumbra::world::GameSession;
    InstallPresets();
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    {
        GameSession session;
        session.SetRootPath(Luminumbra::Filesystem::GenericUtf8PathString(root) + "/");
        session.SetJobSystem(&jobs);
        session.SetAetherStateEnabled(true);
        ASSERT_TRUE(session.CreateWorld("Saved", "1337", "default"));
        ASSERT_NE(session.GetEnergyFieldState(), nullptr);
        session.GetEnergyFieldState()->SetAnchorCell(0, 0);
        session.GetEnergyFieldState()->QueueDeposit(1, 0, 0, 0, 512);
        session.GetEnergyFieldState()->Tick(1);
        constexpr std::size_t kSamples = static_cast<std::size_t>(Luminumbra::CHUNK_SIZE_X + 1) *
                                         (Luminumbra::CHUNK_SIZE_Y + 1) *
                                         (Luminumbra::CHUNK_SIZE_Z + 1);
        for (int x = 0; x < 2; ++x) {
            auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(x, 0, 0));
            chunk->sdf_data.assign(kSamples, -1.0f);
            chunk->mark_voxel_data_dirty();
            ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
        }
        ASSERT_TRUE(session.SaveWorld());
        const auto energy_before = session.GetEnergyFieldState()->CanonicalBytes();
        const auto id = session.GetMetadata().worldId;
        const auto options = session.CaptureWorldPreparationOptions();
        const auto save_directory = session.GetWorldSaveDir();
        const auto before = DiskBytes(root);
        const auto revision = session.WorldRevision();
        auto* world = session.GetWorldSystem();
        world->SetWaterSimWindowCursor(19);
        for (const bool state_only : {false, true}) {
            std::size_t reads = 0;
            const auto prepare = [&](const std::stop_token& token) {
                return state_only ? GameSession::PrepareWorldState(options, save_directory, token)
                                  : GameSession::PrepareWorldLoad(options, id, token);
            };
            {
                CancelPreparationRead observe(0);
                auto prepared = prepare({});
                ASSERT_TRUE(GameSession::WorldPreparationError(*prepared).empty());
                reads = observe.opens;
            }
            ASSERT_GT(reads, 0u);
            // Fixed tiny fixture; cover each actual read, including repeated catalog
            // validation and final staged state reads, without relying on timing.
            ASSERT_LT(reads, 64u);
            for (std::size_t ordinal = 1; ordinal <= reads; ++ordinal) {
                SCOPED_TRACE(ordinal);
                CancelPreparationRead cancel(ordinal);
                std::shared_ptr<GameSession::WorldPreparation> prepared;
                std::jthread worker([&] { prepared = prepare(cancel.stop.get_token()); });
                worker.join();
                EXPECT_TRUE(cancel.stop.stop_requested());
                EXPECT_EQ(cancel.opens, ordinal); // Cancellation must stop further file work.
                EXPECT_EQ(GameSession::WorldPreparationError(*prepared),
                          "Saved-world validation cancelled.");
                EXPECT_FALSE(session.AdoptPreparedWorld(prepared));
                EXPECT_EQ(session.WorldRevision(), revision);
                EXPECT_EQ(session.GetWorldSystem(), world);
                EXPECT_EQ(world->GetWaterSimWindowCursor(), 19u);
                EXPECT_EQ(session.GetEnergyFieldState()->CanonicalBytes(), energy_before);
                EXPECT_EQ(DiskBytes(root), before);
            }
        }
    }
    jobs.shutdown();
}

TEST_F(WorldEntryFilesystem, PreparedRefusalsRetainDiagnosticsAndLiveState) {
    using Luminumbra::world::GameSession;
    InstallPresets();
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    {
        GameSession session;
        session.SetRootPath(Luminumbra::Filesystem::GenericUtf8PathString(root) + "/");
        session.SetJobSystem(&jobs);
        ASSERT_TRUE(session.CreateWorld("Retained", "1337", "default"));
        const auto id = session.GetMetadata().worldId;
        const auto options = session.CaptureWorldPreparationOptions();
        const auto revision = session.WorldRevision();
        auto* world = session.GetWorldSystem();
        auto empty_state = GameSession::PrepareWorldState(options, {});
        EXPECT_EQ(GameSession::WorldPreparationError(*empty_state),
                  "World state directory is empty.");
        EXPECT_FALSE(session.AdoptPreparedWorld(empty_state));
        EXPECT_EQ(session.WorldRevision(), revision);
        EXPECT_EQ(session.GetWorldSystem(), world);
        const auto metadata = session.GetWorldSaveDir() / "world_info.json";
        const auto original = Read(metadata);
        for (const auto& bytes : {std::string("{"), std::string("{\"container_version\":999}")}) {
            Write(metadata, bytes);
            const auto before = DiskBytes(root);
            auto prepared = GameSession::PrepareWorldLoad(options, id);
            const auto error = GameSession::WorldPreparationError(*prepared);
            EXPECT_FALSE(error.empty());
            EXPECT_FALSE(session.AdoptPreparedWorld(prepared));
            EXPECT_EQ(session.GetWorldOpenError(), error);
            EXPECT_EQ(session.WorldRevision(), revision);
            EXPECT_EQ(session.GetWorldSystem(), world);
            EXPECT_EQ(DiskBytes(root), before);
        }
        std::filesystem::remove(metadata);
        std::filesystem::create_directory(metadata);
        auto unreadable = GameSession::PrepareWorldLoad(options, id);
        EXPECT_FALSE(GameSession::WorldPreparationError(*unreadable).empty());
        EXPECT_FALSE(session.AdoptPreparedWorld(unreadable));
        EXPECT_EQ(session.WorldRevision(), revision);
        EXPECT_EQ(session.GetWorldSystem(), world);
        EXPECT_TRUE(std::filesystem::is_directory(metadata));
        std::filesystem::remove(metadata);
        Write(metadata, original);
        for (const bool create : {false, true}) {
            auto prepared = create ? GameSession::PrepareWorldCreation(
                                         options, "No jobs", "1337", "default", nullptr)
                                   : GameSession::PrepareWorldLoad(options, id);
            ASSERT_TRUE(GameSession::WorldPreparationError(*prepared).empty());
            session.SetJobSystem(nullptr);
            EXPECT_FALSE(session.AdoptPreparedWorld(prepared));
            EXPECT_EQ(session.GetWorldOpenError(),
                      create ? "JobSystem not set before creating world."
                             : "JobSystem not set before loading world.");
            session.SetJobSystem(&jobs);
            EXPECT_EQ(session.WorldRevision(), revision);
            EXPECT_EQ(session.GetWorldSystem(), world);
        }
        auto mismatched_options = options;
        mismatched_options.active_regions = false;
        auto mismatched = GameSession::PrepareWorldCreation(
            mismatched_options, "Refused", "1337", "default", nullptr);
        ASSERT_TRUE(GameSession::WorldPreparationError(*mismatched).empty());
        const auto before = DiskBytes(root);
        EXPECT_FALSE(session.AdoptPreparedWorld(mismatched));
        EXPECT_EQ(session.GetWorldOpenError(),
                  "World preparation no longer matches session configuration.");
        EXPECT_EQ(session.WorldRevision(), revision);
        EXPECT_EQ(session.GetWorldSystem(), world);
        EXPECT_EQ(DiskBytes(root), before);
        auto previous_root =
            GameSession::PrepareWorldCreation(options, "Previous root", "1337", "default", nullptr);
        ASSERT_TRUE(GameSession::WorldPreparationError(*previous_root).empty());
        session.SetSaveRootPath(root / "changed-root");
        EXPECT_FALSE(session.AdoptPreparedWorld(previous_root));
        EXPECT_EQ(session.GetWorldOpenError(),
                  "World preparation no longer matches session configuration.");
        EXPECT_EQ(session.WorldRevision(), revision);
        EXPECT_EQ(session.GetWorldSystem(), world);
        EXPECT_EQ(DiskBytes(root), before);
        session.SetSaveRootPath(options.configured_save_root);
        auto missing = GameSession::PrepareWorldLoad(options, "missing-world");
        EXPECT_FALSE(GameSession::WorldPreparationError(*missing).empty());
        EXPECT_FALSE(session.AdoptPreparedWorld(missing));
        auto unavailable = options;
        unavailable.job_system_available = false;
        auto refused =
            GameSession::PrepareWorldCreation(unavailable, "Refused", "1337", "default", nullptr);
        EXPECT_EQ(GameSession::WorldPreparationError(*refused),
                  "JobSystem not set before creating world.");
        EXPECT_FALSE(session.AdoptPreparedWorld(refused));
    }
    jobs.shutdown();
}

TEST_F(WorldEntryFilesystem, CreatesWorldWithActiveRegionsAndFullArtifactJournal) {
    InstallPresets();
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    CreatedWorldArtifacts artifacts;
    std::filesystem::path save;
    {
        Luminumbra::world::GameSession session;
        session.SetRootPath(root.generic_string() + "/");
        session.SetJobSystem(&jobs);
        ASSERT_TRUE(session.ActiveRegionsEnabled());
        ASSERT_TRUE(session.CreateWorld("New world", "1337", "default", nullptr, artifacts))
            << session.GetWorldOpenError();
        save = root / "worlds/saves" / session.GetMetadata().worldId;
        const auto bytes = DiskBytes(save);
        EXPECT_EQ(bytes.size(), 6u);
        EXPECT_TRUE(bytes.contains("preset.json"));
        EXPECT_TRUE(bytes.contains("world_info.json"));
        EXPECT_TRUE(bytes.contains("chunks"));
        EXPECT_TRUE(bytes.contains("chunks/region"));
        EXPECT_TRUE(bytes.contains("chunks/region/active-regions.arl"));
        EXPECT_TRUE(bytes.contains("chunks/region/snapshot.json"));
        EXPECT_FALSE(Luminumbra::Persistence::journal_pending(save));
    }
    jobs.shutdown();
    EXPECT_TRUE(artifacts.rollback().empty());
    EXPECT_FALSE(std::filesystem::exists(save));
}

TEST_F(WorldEntryFilesystem, UnicodeSaveRootCreatesAndReloadsTheExactPresetAndSave) {
    InstallPresets();
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    CreatedWorldArtifacts artifacts;
    {
        Luminumbra::world::GameSession session;
        session.SetRootPath(root.generic_string() + "/");
        const auto save_root = root / std::filesystem::path(u8"世界-é");
        session.SetSaveRootPath(save_root);
        session.SetJobSystem(&jobs);
        ASSERT_TRUE(session.CreateWorld("New world", "1337", "default", nullptr, artifacts))
            << session.GetWorldOpenError();
        const auto id = session.GetMetadata().worldId;
        const auto preset = session.PresetContentHash();
        const auto save = save_root / "worlds/saves" / id;
        const auto before = DiskBytes(save);
        ASSERT_EQ(before.size(), 6u);
        ASSERT_TRUE(before.contains("chunks/region/snapshot.json"));
        ASSERT_FALSE(preset.empty());
        artifacts.release();
        ASSERT_TRUE(session.LoadWorld(id)) << session.GetWorldOpenError();
        EXPECT_EQ(session.PresetContentHash(), preset);
        EXPECT_EQ(session.GetSaveRootPath(), save_root);
        EXPECT_EQ(session.GetMetadata().worldId, id);
        EXPECT_EQ(DiskBytes(save), before);
    }
    jobs.shutdown();
}

TEST_F(WorldEntryFilesystem, JournalMetadataNormalizesCalendarsAndPreservesWaterCursor) {
    using Luminumbra::Persistence::WorldSaveService;
    using Luminumbra::world::WorldCalendar;
    using Luminumbra::world::WorldClock;
    for (const bool active_regions : {false, true}) {
        SCOPED_TRACE(active_regions);
        const auto save = root / (active_regions ? "simulation-calendar" : "render-calendar");
        CreatedWorldArtifacts artifacts;
        artifacts.create_root(save);
        const WorldClock clock(0, WorldCalendar::Legacy());
        nlohmann::json metadata = {{"waterSimCursor", 65}};
        if (active_regions) {
            metadata["simulationTick"] = 0u;
            // Version 2 expressing legacy values normalizes to the legacy encoding.
            metadata["calendar"] = clock.calendar_metadata();
        } else {
            metadata["renderCalendar"] = clock.calendar_metadata();
        }
        ASSERT_TRUE(WorldSaveService::save_metadata(metadata.dump(), save, nullptr, &artifacts));
        const auto saved = nlohmann::json::parse(Read(save / "world_info.json"));
        EXPECT_EQ(saved.at("waterSimCursor"), 65u);
        auto expected = metadata;
        if (active_regions)
            clock.write_metadata(expected);
        EXPECT_EQ(saved, expected);
        EXPECT_EQ(Read(save / "world_info.json"), expected.dump(4) + "\n");
        EXPECT_TRUE(artifacts.rollback().empty());
        EXPECT_FALSE(std::filesystem::exists(save));
    }
}

TEST_F(WorldEntryFilesystem, JournalEntryReloadAndExportRetainCalendarAndWaterCursor) {
    InstallPresets();
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    for (const bool active_regions : {false, true}) {
        SCOPED_TRACE(active_regions);
        Luminumbra::world::GameSession session;
        session.SetRootPath(Luminumbra::Filesystem::GenericUtf8PathString(root) + "/");
        session.SetJobSystem(&jobs);
        session.SetActiveRegionsEnabled(active_regions);
        CreatedWorldArtifacts artifacts;
        ASSERT_TRUE(session.CreateWorld("Cursor world", "1337", "default", nullptr, artifacts))
            << session.GetWorldOpenError();
        artifacts.release();
        ASSERT_TRUE(session.LoadWorldState());
        const auto calendar = session.GetRenderCalendar();
        session.GetWorldSystem()->SetWaterSimWindowCursor(65);
        ASSERT_TRUE(session.SaveWorld());
        const auto id = session.GetMetadata().worldId;
        ASSERT_TRUE(session.LoadWorld(id)) << session.GetWorldOpenError();
        EXPECT_EQ(session.GetRenderCalendar(), calendar);
        EXPECT_EQ(session.GetWorldSystem()->GetWaterSimWindowCursor(), 65u);
        ASSERT_TRUE(session.LoadWorldFrom(id, root)) << session.GetWorldOpenError();
        EXPECT_EQ(session.GetRenderCalendar(), calendar);
        EXPECT_EQ(session.GetWorldSystem()->GetWaterSimWindowCursor(), 65u);
        const auto exported = root / (active_regions ? "active-export" : "render-export");
        ASSERT_TRUE(session.SaveWorldStateTo(exported, nullptr, true));
        session.GetWorldSystem()->SetWaterSimWindowCursor(19);
        ASSERT_TRUE(session.LoadWorldStateFrom(exported)) << session.GetWorldOpenError();
        EXPECT_EQ(session.GetRenderCalendar(), calendar);
        EXPECT_EQ(session.GetWorldSystem()->GetWaterSimWindowCursor(), 65u);
    }
    jobs.shutdown();
}

TEST_F(WorldEntryFilesystem, StagedReadRefusalKeepsCandidateOpensLiveAndTearsDownSnapshotRead) {
    InstallPresets();
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    std::string id;
    std::filesystem::path save;
    {
        Luminumbra::world::GameSession source;
        source.SetRootPath(Luminumbra::Filesystem::GenericUtf8PathString(root) + "/");
        source.SetJobSystem(&jobs);
        ASSERT_TRUE(source.CreateWorld("Default scheduler", "1337", "default"));
        id = source.GetMetadata().worldId;
        save = source.GetWorldSaveDir();
    }
    const auto before = DiskBytes(root);
    // Catalog admission succeeds; the staged reader then refuses an explicit
    // scheduler mismatch. Exercise canonical, immutable-root and snapshot paths.
    for (unsigned path = 0; path < 3; ++path) {
        SCOPED_TRACE(path);
        Luminumbra::world::GameSession reader;
        reader.SetRootPath(Luminumbra::Filesystem::GenericUtf8PathString(root) + "/");
        reader.SetJobSystem(&jobs);
        reader.SetRegionSchedulerConfig({.work_limit = 0, .hold_ticks = 2, .reduced_shift = 1});
        ASSERT_TRUE(reader.CreateTransientWorld("Retained menu", "1337", "default"));
        const auto revision = reader.WorldRevision();
        reader.GetWorldSystem()->SetWaterSimWindowCursor(19);
        const bool loaded = path == 0   ? reader.LoadWorld(id)
                            : path == 1 ? reader.LoadWorldFrom(id, root)
                                        : reader.LoadWorldStateFrom(save);
        EXPECT_FALSE(loaded);
        EXPECT_NE(reader.GetWorldOpenError().find("persisted region scheduler differs"),
                  std::string::npos);
        // Candidate opens keep the live world when final state validation
        // refuses; only the direct snapshot read tears it down.
        if (path == 2) {
            EXPECT_NE(reader.WorldRevision(), revision);
            EXPECT_FALSE(reader.LoadWorldState());
        } else {
            EXPECT_EQ(reader.WorldRevision(), revision);
        }
        EXPECT_EQ(reader.GetWorldSystem()->GetWaterSimWindowCursor(), 19u);
        EXPECT_FALSE(reader.SaveWorld());
        EXPECT_FALSE(reader.SaveWorldState());
        EXPECT_FALSE(reader.SaveWorldStateTo(root / "refused-export", nullptr, true));
        EXPECT_EQ(DiskBytes(root), before);
    }
    jobs.shutdown();
}

TEST_F(WorldEntryFilesystem, WorldRevisionDistinguishesRefusalFromPartialReplacement) {
    InstallPresets();
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    CreatedWorldArtifacts artifacts;
    {
        Luminumbra::world::GameSession session;
        session.SetRootPath(root.generic_string() + "/");
        session.SetJobSystem(&jobs);
        const auto initial = session.WorldRevision();
        ASSERT_TRUE(session.CreateTransientWorld("Menu Vista", "424242", "default"));
        const auto menu_revision = session.WorldRevision();
        EXPECT_NE(menu_revision, initial);
        const auto* menu_world = session.GetWorldSystem();
        const auto menu_spawn = session.GetMetadata().spawnPoint;
        EXPECT_FALSE(session.LoadWorld("missing-world"));
        EXPECT_EQ(session.WorldRevision(), menu_revision);
        EXPECT_EQ(session.GetWorldSystem(), menu_world);
        EXPECT_EQ(session.GetMetadata().spawnPoint, menu_spawn);
        EXPECT_FALSE(session.CreateWorld("Refused", "1337", "../default"));
        EXPECT_EQ(session.WorldRevision(), menu_revision);
        EXPECT_EQ(session.GetWorldSystem(), menu_world);

        // A false return does not imply that the menu world survived: metadata
        // publication happens after the old world has been released.
        Luminumbra::Persistence::WorldSaveService::set_before_active_regions_replace_for_testing(
            [] { return false; });
        const bool created =
            session.CreateWorld("Interrupted", "1337", "default", nullptr, artifacts);
        Luminumbra::Persistence::WorldSaveService::set_before_active_regions_replace_for_testing(
            nullptr);
        EXPECT_FALSE(created);
        EXPECT_NE(session.WorldRevision(), menu_revision);
        EXPECT_EQ(session.GetMetadata().name, "Interrupted");
    }
    jobs.shutdown();
    EXPECT_TRUE(artifacts.rollback().empty());
}

TEST_F(WorldEntryFilesystem, CancelAfterOpenWorldRemovesPublishedCreationArtifacts) {
    WorldEntry<Request> entry;
    CreatedWorldArtifacts artifacts;
    const auto save = root / "created-world";
    std::filesystem::create_directory(root / "existing-world");
    Write(root / "existing-world/world_info.json", "existing metadata\n");
    unsigned stages = 0;
    auto run = [&](WorldEntryStage stage, const Request&) {
        ++stages;
        if (stage == WorldEntryStage::OpenWorld)
            Publish(artifacts, save);
        return WorldEntryResult::Complete;
    };
    auto drain = [&] {
        EXPECT_TRUE(artifacts.rollback().empty());
        return WorldEntryResult::Complete;
    };
    ASSERT_TRUE(entry.request(Request{.identity = "created-world", .lifetime = {}}));
    entry.loading_painted();
    for (unsigned index = 0; index < 3; ++index)
        entry.advance(run, drain);
    ASSERT_EQ(entry.stage(), WorldEntryStage::RestoreState);
    ASSERT_EQ(Read(save / "preset.json"), "preset bytes\n");
    ASSERT_EQ(Read(save / "world_info.json"), "metadata bytes\n");
    ASSERT_EQ(Read(save / "chunks/region/active-regions.arl"), "ledger bytes\n");
    entry.cancel_input(true);
    entry.cancel_input(false);
    entry.advance(run, drain);
    EXPECT_EQ(entry.state(), WorldEntryState::Cancelled);
    EXPECT_EQ(stages, 3u);
    EXPECT_FALSE(std::filesystem::exists(save));
    EXPECT_EQ(Read(root / "existing-world/world_info.json"), "existing metadata\n");
    EXPECT_TRUE(artifacts.rollback().empty()); // Cleanup is idempotent.
}

TEST_F(WorldEntryFilesystem, LoadCancelLeavesExistingSaveUntouched) {
    using Luminumbra::Persistence::WorldSaveService;
    const Luminumbra::Persistence::SyntheticSaveSpec spec{
        .seed = 1337,
        .chunk_count = 1,
        .edit_count = 1,
        .payload = Luminumbra::Persistence::FixturePayload::Flat,
        .world_id = "existing-world",
        .world_name = "Existing world",
        .preset_source =
            std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets/default.json"};
    const auto fixture = Luminumbra::Persistence::WriteSyntheticSave(spec, root);
    ASSERT_TRUE(fixture.ok) << fixture.error;
    const auto& save = fixture.save_dir;
    auto metadata = nlohmann::json::parse(Read(save / "world_info.json"));
    Luminumbra::world::WorldClock clock;
    clock.write_metadata(metadata);
    Luminumbra::world::ActiveRegionLedger ledger;
    ASSERT_TRUE(ledger.try_set_local_anchor(Luminumbra::Vec3(0.0f, 100.0f, 0.0f)));
    ASSERT_TRUE(WorldSaveService::save_metadata_and_active_regions(metadata.dump(), ledger, save));
    const auto before = DiskBytes(save);
    ASSERT_TRUE(before.contains("preset.json"));
    ASSERT_TRUE(before.contains("world_info.json"));
    ASSERT_TRUE(before.contains("chunks/region/active-regions.arl"));

    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    auto session = std::make_unique<Luminumbra::world::GameSession>();
    session->SetRootPath(root.generic_string() + "/");
    session->SetJobSystem(&jobs);
    session->SetActiveRegionsEnabled(true);
    WorldEntry<Request> entry;
    CreatedWorldArtifacts artifacts; // Loading never claims creation artifacts.
    unsigned stages = 0;
    auto run = [&](WorldEntryStage stage, const Request& request) {
        ++stages;
        if (stage == WorldEntryStage::OpenWorld) {
            const bool loaded = session->LoadWorld(request.identity);
            EXPECT_TRUE(loaded) << session->GetWorldOpenError();
            if (!loaded)
                return WorldEntryResult::Failed;
        }
        return WorldEntryResult::Complete;
    };
    auto drain = [&] {
        session.reset(); // Real session teardown drains readers before rollback.
        EXPECT_TRUE(artifacts.rollback().empty());
        return WorldEntryResult::Complete;
    };
    ASSERT_TRUE(entry.request(Request{.identity = spec.world_id, .lifetime = {}}));
    entry.loading_painted();
    for (unsigned index = 0; index < 3; ++index)
        entry.advance(run, drain);
    ASSERT_EQ(entry.stage(), WorldEntryStage::RestoreState);
    ASSERT_EQ(session->GetLastLoadedChunkCount(), 1u);
    EXPECT_EQ(session->GetMetadata().worldId, spec.world_id);
    EXPECT_EQ(DiskBytes(save), before);
    entry.cancel_input(true);
    entry.cancel_input(false);
    entry.advance(run, drain);
    EXPECT_EQ(entry.state(), WorldEntryState::Cancelled);
    EXPECT_EQ(stages, 3u);
    EXPECT_EQ(session, nullptr);
    jobs.shutdown();
    EXPECT_EQ(DiskBytes(save), before);
}

TEST_F(WorldEntryFilesystem, CancelRollsBackRealMetadataAndActiveRegionWriters) {
    WorldEntry<Request> entry;
    CreatedWorldArtifacts artifacts;
    const auto save = root / "created-world";
    auto run = [&](WorldEntryStage stage, const Request&) {
        if (stage == WorldEntryStage::OpenWorld) {
            artifacts.create_root(save);
            artifacts.write_file(save / "preset.json", "preset bytes\n");
            Luminumbra::world::WorldClock clock;
            Luminumbra::world::ActiveRegionLedger ledger;
            EXPECT_TRUE(ledger.try_set_local_anchor(Luminumbra::Vec3(0.0f, 100.0f, 0.0f)));
            nlohmann::json metadata = {
                {"container_version", Luminumbra::Persistence::WorldSaveService::kContainerVersion},
                {"spawnPoint", {{"x", 0.0f}, {"y", 100.0f}, {"z", 0.0f}}}};
            clock.write_metadata(metadata);
            std::vector<std::string> errors;
            EXPECT_TRUE(Luminumbra::Persistence::WorldSaveService::save_metadata_and_active_regions(
                metadata.dump() + "\n", ledger, save, &errors, &artifacts));
            EXPECT_TRUE(errors.empty()) << (errors.empty() ? "" : errors.front());
        }
        return WorldEntryResult::Complete;
    };
    auto drain = [&] {
        EXPECT_TRUE(artifacts.rollback().empty());
        return WorldEntryResult::Complete;
    };
    ASSERT_TRUE(entry.request(Request{.identity = "created-world", .lifetime = {}}));
    entry.loading_painted();
    for (unsigned index = 0; index < 3; ++index)
        entry.advance(run, drain);
    ASSERT_TRUE(std::filesystem::is_regular_file(save / "world_info.json"));
    ASSERT_TRUE(std::filesystem::is_regular_file(save / "chunks/region/active-regions.arl"));
    entry.cancel_input(true);
    entry.cancel_input(false);
    entry.advance(run, drain);
    EXPECT_EQ(entry.state(), WorldEntryState::Cancelled);
    EXPECT_FALSE(std::filesystem::exists(save));
}

TEST_F(WorldEntryFilesystem, RollbackRefusesUntrackedArtifactsBeforeDeletingAnyOwnedFile) {
    CreatedWorldArtifacts artifacts;
    const auto save = root / "created-world";
    Publish(artifacts, save);
    const auto foreign = save / "chunks/region/foreign.bin";
    Write(foreign, "untracked bytes\n");
    const auto error = artifacts.rollback();
    EXPECT_NE(error.find("Untracked world artifact"), std::string::npos);
    EXPECT_EQ(Read(foreign), "untracked bytes\n");
    EXPECT_EQ(Read(save / "preset.json"), "preset bytes\n");
    EXPECT_EQ(Read(save / "world_info.json"), "metadata bytes\n");
    EXPECT_EQ(Read(save / "chunks/region/active-regions.arl"), "ledger bytes\n");
    EXPECT_TRUE(std::filesystem::remove(foreign));
    EXPECT_TRUE(artifacts.rollback().empty());
    EXPECT_FALSE(std::filesystem::exists(save));
}

TEST_F(WorldEntryFilesystem, ReplacedRootWithSameSaveLayoutSurvivesRollback) {
    CreatedWorldArtifacts artifacts;
    const auto save = root / "created-world";
    Publish(artifacts, save);
    const auto existing = root / "existing-world";
    CreatedWorldArtifacts existing_artifacts;
    Publish(existing_artifacts, existing);
    existing_artifacts.release();
    Write(existing / "world_info.json", "existing save metadata\n");
    const auto before = DiskBytes(existing);
    const auto created_before = DiskBytes(save);
    const auto moved = root / "moved-creation";
    std::filesystem::rename(save, moved);
    std::filesystem::rename(existing, save);
    EXPECT_NE(artifacts.rollback().find("changed identity"), std::string::npos);
    EXPECT_EQ(DiskBytes(save), before);
    EXPECT_EQ(DiskBytes(moved), created_before);
}

TEST_F(WorldEntryFilesystem, DurableWriterPinsAllowAncestorRenameAndRollbackRetry) {
    using Luminumbra::Persistence::WorldSaveService;
    CreatedWorldArtifacts artifacts;
    const auto save = root / "created-world";
    artifacts.create_root(save);
    artifacts.create_directory(save / "chunks");
    artifacts.create_directory(save / "chunks/region");
    std::vector<std::string> errors;
    ASSERT_TRUE(WorldSaveService::write_file_durably(
        save / "preset.json", "preset bytes\n", &errors, &artifacts));
    ASSERT_TRUE(
        WorldSaveService::write_file_durably(save / "world_info.json", "", &errors, &artifacts));
    ASSERT_TRUE(WorldSaveService::write_file_durably(
        save / "chunks/region/active-regions.arl", "ledger bytes\n", &errors, &artifacts));
    ASSERT_TRUE(errors.empty());
    const auto before = DiskBytes(save);
    const auto moved = root / "moved-creation";
    std::filesystem::rename(save, moved);
    ASSERT_TRUE(std::filesystem::create_directory(save));
    EXPECT_NE(artifacts.rollback().find("changed identity"), std::string::npos);
    EXPECT_EQ(DiskBytes(moved), before);
    ASSERT_TRUE(std::filesystem::remove(save));
    std::filesystem::rename(moved, save);
    EXPECT_TRUE(artifacts.rollback().empty());
    EXPECT_FALSE(std::filesystem::exists(save));
}

TEST_F(WorldEntryFilesystem, UncreatedFileReservationDoesNotClaimLaterFile) {
    CreatedWorldArtifacts artifacts;
    const auto save = root / "created-world";
    artifacts.create_root(save);
    const auto file = save / "preset.json";
    artifacts.reserve_file(file);
    Write(file, "existing file created by another writer\n");
    EXPECT_NE(artifacts.rollback().find("changed identity"), std::string::npos);
    EXPECT_EQ(Read(file), "existing file created by another writer\n");
}

TEST_F(WorldEntryFilesystem, DurablePresetWriterPublishesOwnedFileAndRollsBack) {
    using Luminumbra::Persistence::WorldSaveService;
    CreatedWorldArtifacts artifacts;
    const auto save = root / "created-world";
    artifacts.create_root(save);
    std::vector<std::string> errors;
    ASSERT_TRUE(WorldSaveService::write_file_durably(
        save / "preset.json", "preset bytes\n", &errors, &artifacts));
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(Read(save / "preset.json"), "preset bytes\n");
    EXPECT_EQ(DiskBytes(save).size(), 1u);
    EXPECT_TRUE(artifacts.rollback().empty());
    EXPECT_FALSE(std::filesystem::exists(save));
}

TEST_F(WorldEntryFilesystem, UnicodeNativePublicationRetainsEveryWriterAndRollbackIdentity) {
    using Luminumbra::Filesystem::PathFromUtf8;
    using Luminumbra::Persistence::WorldSaveService;
    CreatedWorldArtifacts artifacts;
    const auto save = root / std::filesystem::path(u8"世界-é");
    artifacts.create_root(save);
    artifacts.create_directory(save / "chunks");
    artifacts.create_directory(save / "chunks/region");
    const std::array<std::filesystem::path, 3> names{std::filesystem::path(u8"预设.json"),
                                                     "world_info.json",
                                                     "chunks/region/active-regions.arl"};
    std::vector<std::string> errors;
    for (const auto& name : names) {
        // Exercise generic caller spelling as well as native UTF-16 names.
        const auto path = PathFromUtf8(Luminumbra::Filesystem::GenericUtf8PathString(save / name));
        ASSERT_TRUE(
            WorldSaveService::write_file_durably(path, "owned bytes\n", &errors, &artifacts));
        EXPECT_EQ(Read(path), "owned bytes\n");
    }
    EXPECT_TRUE(errors.empty());
    const auto before = DiskBytes(save);
    EXPECT_EQ(before.size(), 5u);
    const auto foreign = save / std::filesystem::path(u8"未知.bin");
    Write(foreign, "protected bytes\n");
    EXPECT_NE(artifacts.rollback().find("Untracked world artifact"), std::string::npos);
    EXPECT_EQ(Read(foreign), "protected bytes\n");
    ASSERT_TRUE(std::filesystem::remove(foreign));
    const auto moved = root / std::filesystem::path(u8"移动世界");
    std::filesystem::rename(save, moved);
    ASSERT_TRUE(std::filesystem::create_directory(save));
    EXPECT_NE(artifacts.rollback().find("changed identity"), std::string::npos);
    EXPECT_EQ(DiskBytes(moved), before);
    ASSERT_TRUE(std::filesystem::remove(save));
    std::filesystem::rename(moved, save);
    EXPECT_TRUE(artifacts.rollback().empty());
    EXPECT_FALSE(std::filesystem::exists(save));
}

TEST_F(WorldEntryFilesystem, UnicodeDurableWriterFailureReportsThePathAndRetainsOwnedStage) {
    using Luminumbra::Persistence::WorldSaveService;
    CreatedWorldArtifacts artifacts;
    const auto save = root / std::filesystem::path(u8"世界-é");
    const auto file = save / std::filesystem::path(u8"预设.json");
    artifacts.create_root(save);
    WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    struct ResetInterruption {
        ~ResetInterruption() {
            WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
        }
    } reset;
    std::vector<std::string> errors;
    EXPECT_FALSE(WorldSaveService::write_file_durably(file, "owned bytes\n", &errors, &artifacts));
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_NE(errors.front().find(Luminumbra::Filesystem::GenericUtf8PathString(file)),
              std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(file));
    const auto stages = DiskBytes(save);
    ASSERT_EQ(stages.size(), 1u);
    EXPECT_EQ(stages.begin()->second, "owned bytes\n");
    EXPECT_TRUE(artifacts.rollback().empty());
    EXPECT_FALSE(std::filesystem::exists(save));
}

TEST_F(WorldEntryFilesystem, FailedFinalLedgerReservationRollsBackEveryOwnedArtifact) {
    using Luminumbra::Persistence::WorldSaveService;
    CreatedWorldArtifacts artifacts;
    const auto save = root / "created-world";
    artifacts.create_root(save);
    std::vector<std::string> errors;
    ASSERT_TRUE(WorldSaveService::write_file_durably(
        save / "preset.json", "preset bytes\n", &errors, &artifacts));
    // Consume three slots with owned partial files so the final ledger temporary
    // reservation exhausts the journal, after metadata and both directories exist.
    artifacts.write_file(save / "partial.bin", "partial bytes\n");
    artifacts.write_file(save / "partial-second.bin", "partial bytes\n");
    artifacts.write_file(save / "partial-third.bin", "partial bytes\n");
    const Luminumbra::world::WorldClock clock;
    Luminumbra::world::ActiveRegionLedger ledger;
    ASSERT_TRUE(ledger.try_set_local_anchor(Luminumbra::Vec3(0.0f, 100.0f, 0.0f)));
    nlohmann::json metadata = {{"container_version", WorldSaveService::kContainerVersion},
                               {"spawnPoint", {{"x", 0.0f}, {"y", 100.0f}, {"z", 0.0f}}}};
    clock.write_metadata(metadata);
    EXPECT_FALSE(WorldSaveService::save_metadata_and_active_regions(
        metadata.dump() + "\n", ledger, save, &errors, &artifacts));
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_EQ(errors.front(),
              "Cannot save active-region ledger: Invalid world artifact journal capacity.");
    EXPECT_EQ(Read(save / "preset.json"), "preset bytes\n");
    EXPECT_EQ(Read(save / "partial.bin"), "partial bytes\n");
    EXPECT_EQ(Read(save / "partial-second.bin"), "partial bytes\n");
    EXPECT_EQ(Read(save / "partial-third.bin"), "partial bytes\n");
    EXPECT_TRUE(std::filesystem::is_regular_file(save / "world_info.json"));
    EXPECT_TRUE(std::filesystem::is_directory(save / "chunks/region"));
    EXPECT_FALSE(std::filesystem::exists(save / "chunks/region/active-regions.arl"));
    EXPECT_TRUE(artifacts.rollback().empty());
    EXPECT_FALSE(std::filesystem::exists(save));
    EXPECT_TRUE(artifacts.rollback().empty());
}

TEST_F(WorldEntryFilesystem, InterruptedDurablePresetWriterRetainsOwnedTemporaryFile) {
    using Luminumbra::Persistence::WorldSaveService;
    CreatedWorldArtifacts artifacts;
    const auto save = root / "created-world";
    artifacts.create_root(save);
    WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    struct ResetInterruption {
        ~ResetInterruption() {
            WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
        }
    } reset;
    std::vector<std::string> errors;
    EXPECT_FALSE(WorldSaveService::write_file_durably(
        save / "preset.json", "preset bytes\n", &errors, &artifacts));
    EXPECT_FALSE(errors.empty());
    EXPECT_FALSE(std::filesystem::exists(save / "preset.json"));
    const auto bytes = DiskBytes(save);
    ASSERT_EQ(bytes.size(), 1u);
    EXPECT_TRUE(bytes.begin()->first.starts_with("preset.json.tmp."));
    EXPECT_EQ(bytes.begin()->second, "preset bytes\n");
    EXPECT_TRUE(artifacts.rollback().empty());
    EXPECT_FALSE(std::filesystem::exists(save));
}

TEST_F(WorldEntryFilesystem, DurablePresetWriterRefusesExistingDestination) {
    using Luminumbra::Persistence::WorldSaveService;
    CreatedWorldArtifacts artifacts;
    const auto save = root / "created-world";
    artifacts.create_root(save);
    Write(save / "preset.json", "existing preset\n");
    EXPECT_THROW(WorldSaveService::write_file_durably(
                     save / "preset.json", "new preset\n", nullptr, &artifacts),
                 std::runtime_error);
    EXPECT_EQ(Read(save / "preset.json"), "existing preset\n");
    EXPECT_FALSE(artifacts.rollback().empty());
    EXPECT_EQ(Read(save / "preset.json"), "existing preset\n");
}

TEST_F(WorldEntryFilesystem, InterruptedMetadataWriterRetainsOwnedTemporaryFileUntilRollback) {
    using Luminumbra::Persistence::WorldSaveService;
    CreatedWorldArtifacts artifacts;
    const auto save = root / "created-world";
    artifacts.create_root(save);
    artifacts.write_file(save / "preset.json", "preset bytes\n");
    WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    // The writer consumes the interruption before returning; always clear it
    // on exceptional exits as well so later tests see the default writer.
    struct ResetInterruption {
        ~ResetInterruption() {
            WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
        }
    } reset;
    std::vector<std::string> errors;
    EXPECT_FALSE(WorldSaveService::save_metadata("metadata bytes\n", save, &errors, &artifacts));
    ASSERT_FALSE(errors.empty());
    EXPECT_EQ(errors.front(), "interrupted before metadata replacement");
    EXPECT_FALSE(std::filesystem::exists(save / "world_info.json"));
    const auto bytes = DiskBytes(save);
    ASSERT_EQ(bytes.size(), 2u); // Preset plus the unpublished temporary file.
    unsigned temporary_files = 0;
    for (const auto& [name, contents] : bytes) {
        if (name.starts_with("world_info.json.tmp.")) {
            ++temporary_files;
            EXPECT_EQ(contents, "metadata bytes\n");
        }
    }
    EXPECT_EQ(temporary_files, 1u);
    EXPECT_TRUE(artifacts.rollback().empty());
    EXPECT_FALSE(std::filesystem::exists(save));
}

TEST_F(WorldEntryFilesystem, SameTypeFileReplacementSurvivesRollback) {
    CreatedWorldArtifacts artifacts;
    const auto save = root / "created-world";
    Publish(artifacts, save);
    const auto metadata = save / "world_info.json";
    // Keep the original inode allocated, including on filesystems that eagerly reuse IDs.
    const auto moved = root / "original-metadata";
    std::filesystem::rename(metadata, moved);
    const auto replacement = root / "existing-metadata";
    Write(replacement, "existing metadata replacement\n");
    std::filesystem::rename(replacement, metadata);
    const auto before = DiskBytes(save);
    EXPECT_NE(artifacts.rollback().find("changed identity"), std::string::npos);
    EXPECT_EQ(DiskBytes(save), before);
    EXPECT_EQ(Read(moved), "metadata bytes\n");
    EXPECT_EQ(Read(metadata), "existing metadata replacement\n");
}

TEST_F(WorldEntryFilesystem, JournalRefusesExistingSaveAndPathsOutsideOwnedDirectories) {
    const auto existing = root / "existing-world";
    std::filesystem::create_directory(existing);
    Write(existing / "world_info.json", "existing metadata\n");
    CreatedWorldArtifacts refused;
    EXPECT_THROW(refused.create_root(existing), std::runtime_error);
    EXPECT_TRUE(refused.rollback().empty());
    EXPECT_EQ(Read(existing / "world_info.json"), "existing metadata\n");

    CreatedWorldArtifacts artifacts;
    const auto save = root / "created-world";
    artifacts.create_root(save);
    EXPECT_THROW(artifacts.reserve_file(save / "../outside.bin"), std::runtime_error);
    EXPECT_THROW(artifacts.reserve_file(existing / "world_info.json"), std::runtime_error);
    EXPECT_TRUE(artifacts.rollback().empty());
    EXPECT_EQ(Read(existing / "world_info.json"), "existing metadata\n");
}

TEST_F(WorldEntryFilesystem, ChangedArtifactTypeRefusesRollbackAndReleaseKeepsPublishedSave) {
    CreatedWorldArtifacts artifacts;
    const auto save = root / "created-world";
    artifacts.create_root(save);
    artifacts.reserve_file(save / "preset.json");
    std::filesystem::create_directory(save / "preset.json");
    EXPECT_NE(artifacts.rollback().find("changed type"), std::string::npos);
    EXPECT_TRUE(std::filesystem::is_directory(save / "preset.json"));
    artifacts.release();
    EXPECT_TRUE(artifacts.rollback().empty());
    EXPECT_TRUE(std::filesystem::is_directory(save / "preset.json"));
}

std::string ReadSource(const std::filesystem::path& relative) {
    std::ifstream input(std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / relative);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

// main() owns the real GL/UI services. Guard its wiring here while exercising
// the scheduling contract above without those services or a wall clock.
TEST(WorldEntryWiring, CreateAndLoadCallbacksOnlyRecordRequests) {
    const auto source = ReadSource("src/luminumbra_client/main_client.cpp");
    const auto create = source.find("g_uiManager->SetWorldCreationCallback(");
    const auto load = source.find("g_uiManager->SetLoadWorldCallback(", create);
    const auto end = source.find("g_uiManager->SetSavedWorldList(", load);
    ASSERT_NE(create, std::string::npos);
    ASSERT_NE(load, std::string::npos);
    ASSERT_NE(end, std::string::npos);
    for (const auto& callback :
         {source.substr(create, load - create), source.substr(load, end - load)}) {
        EXPECT_NE(callback.find("world_entry.request("), std::string::npos);
        for (const auto* heavy : {"CreateWorld(",
                                  "LoadWorld(",
                                  "LoadWorldState(",
                                  "prepare_world_entry(",
                                  "finish_world_entry(",
                                  "start_world_creation(",
                                  "start_world_load("})
            EXPECT_EQ(callback.find(heavy), std::string::npos) << heavy;
    }
    const auto loading = source.find("case GameState::WORLD_LOADING:");
    const auto present = source.find(
        "LUMIN_PROFILE_WAIT(\"Client present wait\", glfwSwapBuffers(window));", loading);
    const auto painted = source.find("world_entry.loading_painted();");
    ASSERT_NE(present, std::string::npos);
    ASSERT_NE(painted, std::string::npos);
    EXPECT_GT(painted, present);
    EXPECT_NE(source.find("\"Stages complete\""), std::string::npos);
    EXPECT_NE(source.find("entry_activated_this_frame ? GameState::WORLD_LOADING"),
              std::string::npos);
    const auto ui_update = source.find("g_uiManager->Update();");
    const auto report = source.find("g_uiManager->ShowMessage(world_entry_error);", ui_update);
    ASSERT_NE(report, std::string::npos);
    EXPECT_GT(report, ui_update);
    EXPECT_NE(source.find("RequestLoadDocument(\"main_menu.rml\");",
                          source.find("auto return_entry_to_menu")),
              std::string::npos);
    const auto ui_source = ReadSource("src/luminumbra_client/ui/Rml_UIManager.cpp");
    EXPECT_NE(ui_source.find("m_loadWorldCallback(m_selectedWorldId)"), std::string::npos);
    EXPECT_NE(ui_source.find("GetElementById(\"load_world_btn\")"), std::string::npos);
}
TEST(WorldEntryWiring, CleanupDrainsReadersBeforeReleasingWorldAndSuppressesIncompleteSave) {
    const auto source = ReadSource("src/luminumbra_client/main_client.cpp");
    const auto drain_start = source.find("auto drain_entry =");
    const auto drain_end = source.find("auto return_entry_to_menu =", drain_start);
    ASSERT_NE(drain_start, std::string::npos);
    ASSERT_NE(drain_end, std::string::npos);
    const auto drain = source.substr(drain_start, drain_end - drain_start);
    const auto worker = drain.find("jobSystem.wait(entry_cpu_job)");
    const auto readers = drain.find("prepare_world_entry()");
    const auto geometry = drain.find("renderPipeline.cancel_waterfall_adoption()");
    const auto world = drain.find("world->clear_world(");
    ASSERT_NE(worker, std::string::npos);
    ASSERT_NE(readers, std::string::npos);
    ASSERT_NE(geometry, std::string::npos);
    ASSERT_NE(world, std::string::npos);
    EXPECT_LT(worker, readers);
    EXPECT_LT(readers, geometry);
    EXPECT_LT(geometry, world);
    EXPECT_NE(drain.find("entry_save_allowed = false;"), std::string::npos);
    const auto key_callback = source.find("glfwSetKeyCallback(");
    const auto escape = source.find("key == GLFW_KEY_ESCAPE", key_callback);
    const auto latch = source.find("entry_input->cancel_input(action == GLFW_PRESS)", escape);
    const auto forward =
        source.find("key_callback(callback_window, key, scancode, action, mods)", latch);
    ASSERT_NE(key_callback, std::string::npos);
    ASSERT_NE(escape, std::string::npos);
    ASSERT_NE(latch, std::string::npos);
    ASSERT_NE(forward, std::string::npos);
    EXPECT_LT(latch, forward);
    EXPECT_EQ(source.find("glfwGetKey(window, GLFW_KEY_ESCAPE)"), std::string::npos);
    EXPECT_NE(source.find("entry_input->cancel_input(inside && action == GLFW_PRESS)"),
              std::string::npos);
    EXPECT_NE(source.find("glfwSetWindowCloseCallback(window"), std::string::npos);
    EXPECT_NE(source.find("SetNavigationAwayCallback([&] { world_entry.cancel_input(true); })"),
              std::string::npos);
    const auto mouse_callback = source.find("glfwSetMouseButtonCallback(");
    const auto mouse_latch =
        source.find("entry_input->cancel_input(inside && action == GLFW_PRESS)", mouse_callback);
    const auto mouse_forward =
        source.find("mouse_button_callback(callback_window, button, action, mods)", mouse_latch);
    ASSERT_NE(mouse_callback, std::string::npos);
    ASSERT_NE(mouse_latch, std::string::npos);
    ASSERT_NE(mouse_forward, std::string::npos);
    EXPECT_LT(mouse_latch, mouse_forward);
    const auto callbacks = ReadSource("src/luminumbra_client/app/InputCallbacks.cpp");
    const auto mouse_start = callbacks.find("void mouse_button_callback(");
    const auto mouse_end = callbacks.find("void window_focus_callback(", mouse_start);
    ASSERT_NE(mouse_start, std::string::npos);
    ASSERT_NE(mouse_end, std::string::npos);
    const auto mouse_body = callbacks.substr(mouse_start, mouse_end - mouse_start);
    EXPECT_NE(mouse_body.find("ImGui_ImplGlfw_MouseButtonCallback(window, button, action, mods)"),
              std::string::npos);
    EXPECT_NE(mouse_body.find("Rml_UIManager::MouseButtonCallback(window, button, action, mods)"),
              std::string::npos);
    const auto rollback = drain.find("entry_artifacts.rollback()");
    ASSERT_NE(rollback, std::string::npos);
    EXPECT_LT(world, rollback);
    EXPECT_NE(drain.find("world_entry_error += rollback_error"), std::string::npos);
    const auto shutdown_drain = source.find("world_entry.advance(run_entry_stage, drain_entry)");
    const auto save_guard = source.find("if (entry_save_allowed)", shutdown_drain);
    const auto save = source.find("SaveSessionOnExit(", save_guard);
    ASSERT_NE(shutdown_drain, std::string::npos);
    ASSERT_NE(save_guard, std::string::npos);
    ASSERT_NE(save, std::string::npos);
    EXPECT_LT(shutdown_drain, save_guard);
    EXPECT_LT(save_guard, save);
}

TEST(WorldEntryWiring, EarlyCancellationAndRefusalKeepTheMenuWorldAndBackdrop) {
    const auto source = ReadSource("src/luminumbra_client/main_client.cpp");
    const auto requested = source.find("if (world_entry.state() == WorldEntryState::Requested)");
    const auto snapshot =
        source.find("entry_world_revision = gameSession->WorldRevision();", requested);
    const auto loading =
        source.find("SetGameState(window, gameStateManager, GameState::WORLD_LOADING)", requested);
    ASSERT_NE(requested, std::string::npos);
    ASSERT_NE(snapshot, std::string::npos);
    ASSERT_NE(loading, std::string::npos);
    EXPECT_LT(snapshot, loading);

    for (const auto* region : {"auto refuse_world_entry =", "auto drain_entry ="}) {
        const auto start = source.find(region);
        const auto end = source.find("\n    };", start);
        ASSERT_NE(start, std::string::npos);
        ASSERT_NE(end, std::string::npos);
        const auto body = source.substr(start, end - start);
        const auto guard = body.find("if (gameSession->WorldRevision() != entry_world_revision) {");
        ASSERT_NE(guard, std::string::npos) << region;
        const auto begin = body.find('{', guard);
        std::size_t guard_end = begin + 1;
        unsigned depth = 1;
        for (; guard_end < body.size() && depth != 0; ++guard_end) {
            if (body[guard_end] == '{')
                ++depth;
            else if (body[guard_end] == '}')
                --depth;
        }
        ASSERT_EQ(depth, 0u);
        const auto guarded = body.substr(begin, guard_end - begin);
        const auto outside = body.substr(0, guard) + body.substr(guard_end);
        for (const auto* destructive : {"prepare_world_entry()",
                                        "cancel_waterfall_adoption()",
                                        "clear_world(",
                                        "menu_backdrop_active = false",
                                        "g_playerController.reset()"}) {
            EXPECT_EQ(outside.find(destructive), std::string::npos) << region << destructive;
        }
        EXPECT_NE(guarded.find("menu_backdrop_active = false"), std::string::npos);
        EXPECT_NE(guarded.find("g_playerController.reset()"), std::string::npos);
        if (std::string(region) == "auto drain_entry =") {
            EXPECT_NE(guarded.find("prepare_world_entry()"), std::string::npos);
            EXPECT_NE(guarded.find("cancel_waterfall_adoption()"), std::string::npos);
            EXPECT_NE(guarded.find("clear_world("), std::string::npos);
            const auto wait = body.find("jobSystem.wait(entry_cpu_job)");
            const auto rollback = body.find("entry_artifacts.rollback()");
            ASSERT_NE(wait, std::string::npos);
            ASSERT_NE(rollback, std::string::npos);
            EXPECT_LT(wait, guard);
            EXPECT_GT(rollback, guard_end);
        }
    }
}

} // namespace
