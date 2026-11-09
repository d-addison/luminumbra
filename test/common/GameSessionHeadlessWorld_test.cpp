#include "../support/IndexedSaveFixture.h"
// asset-manifest split. The engine-side GameSession validates
// SIMULATION requirements only (preset and worldgen dependencies); renderer/UI
// asset requirements are caller-supplied by the client. Worldgen dependencies
// are simulation assets and must be available even for a headless host.
#include "../support/PersistenceFileOps.h"
#include "../support/SessionEnergyTick.h"
#include "../support/UniqueTempPath.h"
#include "../support/WorldgenTestData.h"

#include "../support/SystemConfigVariants.h"
#include "entt/entity/fwd.hpp"
#include "luminumbra/core/Types.h"
#include "luminumbra_common/components/CircadianComponents.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/CreatureComponents.h"
#include "luminumbra_common/components/FieldEmitterComponents.h"
#include "luminumbra_common/components/MigratoryComponents.h"
#include "luminumbra_common/components/PlantComponents.h"
#include "luminumbra_common/core/DeterministicMath.h"
#include "luminumbra_common/simulation/SimBudgetTelemetry.h"
#include "luminumbra_common/world/ActiveRegionLedger.h"
#include "luminumbra_common/world/WorldClock.h"
#include "luminumbra_common/world/WorldgenDependencySnapshot.h"
#include "luminumbra_common/world/WorldgenFileReader.h"
#include "nlohmann/json_fwd.hpp"
#include <gtest/gtest.h>
#include <ios>
#include <memory>
#include <optional>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "luminumbra_client/app/FarmSpeciesCache.h"
#include "luminumbra_client/app/SessionExit.h"
#include "luminumbra_client/rendering/TimeOfDayModel.h"
#include "luminumbra_common/ai/CircadianSystem.h"
#include "luminumbra_common/ai/CreatureSpeciesRegistry.h"
#include "luminumbra_common/ai/EcologyHash.h"
#include "luminumbra_common/ai/MigrationSystem.h"
#include "luminumbra_common/ai/ScentField.h"
#include "luminumbra_common/ai/StimulusChannels.h"
#include "luminumbra_common/components/ForagingComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/foliage/SpeciesRegistry.h"
#include "luminumbra_common/net/LockstepSession.h"
#include "luminumbra_common/persistence/PlantPersistence.h"
#include "luminumbra_common/persistence/SavedWorldCatalog.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/replay/RegionScheduleTrace.h"
#include "luminumbra_common/replay/ReplayStream.h"
#include "luminumbra_common/systems/AetherFieldSystem.h"
#include "luminumbra_common/systems/PlantGrowthSystem.h"
#include "luminumbra_common/systems/WaterSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"

#include <nlohmann/json.hpp>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/ecs/EntitySnapshot.h"
#include "luminumbra_common/fields/EnergyFieldState.h"
#include "luminumbra_common/net/LockstepSession.h"
#include "luminumbra_common/persistence/SaveTransaction.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/scripting/LuaState.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/FarAuthorityOverlay.h"
#include "luminumbra_common/world/FarLodStore.h"
#include "luminumbra_common/world/FarWantedSet.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/world/TerrainPresetLoader.h"
#include "luminumbra_common/world/WorldStreamingState.h"

namespace fs = std::filesystem;

namespace {

using Luminumbra::JobSystem;
using Luminumbra::world::GameSession;
using Luminumbra::world::WorldConfigValidationResult;

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

fs::path SourcePresetPath() {
    return fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds" / "atlas" / "presets" / "default.json";
}

// Temp root containing the preset and worldgen data, with no client assets.
// The root is unique per fixture instance so
// concurrent common_tests processes never share (or clobber) a directory; each
// TEST builds ONE instance and reuses it, so within-run path stability still
// holds. The destructor removes the tree.
class HeadlessRoot {
public:
    HeadlessRoot() {
        root_ =
            Luminumbra::TestSupport::CreateUniqueTempDirectory("luminumbra_headless_world_test_");
        fs::create_directories(root_ / "worlds" / "atlas" / "presets");
        fs::copy_file(SourcePresetPath(), root_ / "worlds" / "atlas" / "presets" / "default.json");
        Luminumbra::TestSupport::CopyWorldgenTestData(LUMINUMBRA_SOURCE_ROOT, root_);
    }
    ~HeadlessRoot() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    [[nodiscard]] const fs::path& path() const {
        return root_;
    }
    // GameSession::SetRootPath expects a trailing separator (paths are
    // concatenated, not joined).
    [[nodiscard]] std::string root_string() const {
        return root_.string() + static_cast<char>(fs::path::preferred_separator);
    }

private:
    fs::path root_;
};

TEST(GameSessionHeadlessWorldTest, PlantDeletionFailureFailsLegacySaveWithoutDirtyChunks) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    {
        GameSession session;
        session.SetRootPath(root.root_string());
        session.SetJobSystem(&jobs);
        session.SetActiveRegionsEnabled(false);
        session.SetAetherStateEnabled(false);
        ASSERT_TRUE(session.CreateWorld("Plant deletion failure", "1337", "default"));
        const auto save = session.GetWorldSaveDir();
        const auto path = Luminumbra::Persistence::WorldSaveService::plant_entities_path(save);
        Luminumbra::Ecs::EntityRegistrySnapshot plants;
        plants.entities.push_back(Luminumbra::Ecs::EntitySnapshotRecord{
            .entity_id = 1, .name = "stale plant", .components = {}});
        ASSERT_TRUE(Luminumbra::Persistence::WorldSaveService::save_plant_entities(plants, save));
        for (const auto& chunk : session.GetWorldSystem()->snapshot_streamed_chunks())
            ASSERT_FALSE(chunk->is_voxel_data_dirty());
        Luminumbra::Persistence::detail::set_file_publication_hook_for_testing(
            [](const fs::path& destination, bool removing) {
                return !(removing && destination.filename() == "plant-entities.json");
            });
        Luminumbra::world::WorldStateSaveReport report;
        EXPECT_FALSE(session.SaveWorldStateTo(save, &report));
        EXPECT_EQ(report.chunks_dirty, 0u);
        EXPECT_FALSE(report.saved);
        Luminumbra::Ecs::EntityRegistrySnapshot retained;
        EXPECT_TRUE(Luminumbra::Persistence::WorldSaveService::load_plant_entities(retained, save));
        EXPECT_EQ(Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(retained),
                  Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(plants));
        Luminumbra::Persistence::detail::set_file_publication_hook_for_testing(nullptr);
        EXPECT_TRUE(session.SaveWorldStateTo(save));
        EXPECT_FALSE(fs::exists(path));
    }
    jobs.shutdown();
}

// Fault injection exercises scan errors on every platform and under elevated users.
// No permission-dependent fixture or skipped assertion is needed.
enum class SpeciesScanFailure : std::uint8_t {
    Open,
    Metadata,
    Increment
};

template<SpeciesScanFailure Failure>
class FailingSpeciesDirectoryIterator {
public:
    FailingSpeciesDirectoryIterator() = default;
    FailingSpeciesDirectoryIterator(const fs::path& dir, std::error_code& ec)
        : entry_(dir / "deer.json") {
        if constexpr (Failure == SpeciesScanFailure::Open) {
            ec = std::make_error_code(std::errc::permission_denied);
        } else {
            active_ = true;
            ec.clear();
        }
    }
    class Entry {
        fs::path file_;

    public:
        Entry() = default;
        explicit Entry(fs::path file)
            : file_(std::move(file)) {}
        bool is_regular_file(std::error_code& ec) const {
            if constexpr (Failure == SpeciesScanFailure::Metadata)
                ec = std::make_error_code(std::errc::io_error);
            else
                ec.clear();
            return !ec;
        }
        const fs::path& path() const {
            return file_;
        }
    };
    const Entry& operator*() const {
        return entry_;
    }
    bool operator!=(const FailingSpeciesDirectoryIterator& other) const {
        return active_ != other.active_;
    }
    void increment(std::error_code& ec) {
        ec = std::make_error_code(std::errc::io_error);
        active_ = false;
    }

private:
    Entry entry_;
    bool active_ = false;
};

template<SpeciesScanFailure Failure>
void CheckSpeciesScanFailure(const fs::path& directory, const std::string& diagnostic) {
    luminumbra::ai::CreatureSpeciesRegistry registry;
    std::string parse_error;
    ASSERT_TRUE(registry.AddFromJsonText(R"({"id":"retained"})", parse_error));
    std::vector<std::string> errors{"previous diagnostic"};
    bool io_failed = false;
    std::size_t reads = 0;
    const Luminumbra::World::WorldgenFileReader reader = [&](const fs::path&, std::string&) {
        ++reads;
        return false;
    };
    std::size_t loaded = 1;
    EXPECT_NO_THROW(loaded = registry.LoadFromDirectory<FailingSpeciesDirectoryIterator<Failure>>(
                        directory, errors, reader, &io_failed));
    EXPECT_EQ(loaded, 0u);
    EXPECT_TRUE(io_failed);
    ASSERT_EQ(errors.size(), 2u);
    EXPECT_EQ(errors.front(), "previous diagnostic");
    EXPECT_NE(errors.back().find(diagnostic), std::string::npos);
    EXPECT_EQ(reads, 0u);
    ASSERT_EQ(registry.size(), 1u);
    EXPECT_NE(registry.FindByName("retained"), nullptr);
    // Client/HUD callers omit io_failed and still receive a controlled diagnostic.
    errors.clear();
    EXPECT_NO_THROW(loaded = registry.LoadFromDirectory<FailingSpeciesDirectoryIterator<Failure>>(
                        directory, errors, reader));
    EXPECT_EQ(loaded, 0u);
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_NE(errors.front().find(diagnostic), std::string::npos);
    EXPECT_EQ(reads, 0u);
    EXPECT_EQ(registry.size(), 1u);
}

template<SpeciesScanFailure Failure>
void CheckFoliageScanFailure(const fs::path& directory, const std::string& diagnostic) {
    luminumbra::foliage::SpeciesRegistry registry;
    std::string parse_error;
    ASSERT_TRUE(registry.AddFromJsonText(R"({"id":"retained"})", parse_error));
    std::vector<std::string> errors{"previous diagnostic"};
    bool io_failed = false;
    EXPECT_EQ(registry.LoadFromDirectory<FailingSpeciesDirectoryIterator<Failure>>(
                  directory, errors, &io_failed),
              0u);
    EXPECT_TRUE(io_failed);
    ASSERT_EQ(errors.size(), 2u);
    EXPECT_EQ(errors.front(), "previous diagnostic");
    EXPECT_NE(errors.back().find(diagnostic), std::string::npos);
    ASSERT_EQ(registry.size(), 1u);
    EXPECT_NE(registry.Find("retained"), nullptr);
    errors.clear();
    EXPECT_NO_THROW(
        registry.LoadFromDirectory<FailingSpeciesDirectoryIterator<Failure>>(directory, errors));
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_NE(errors.front().find(diagnostic), std::string::npos);
    // A retry can load valid content exactly once after the failed scan.
    errors.clear();
    EXPECT_EQ(registry.LoadFromDirectory(directory, errors, &io_failed), 1u);
    EXPECT_FALSE(io_failed);
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(registry.size(), 2u);
}

TEST(GameSessionHeadlessWorldTest, FoliageScanFailuresRemainRetryable) {
    const Luminumbra::TestSupport::SpeciesIdentityFixture fixture;
    CheckFoliageScanFailure<SpeciesScanFailure::Open>(fixture.Directory(), "cannot enumerate");
    CheckFoliageScanFailure<SpeciesScanFailure::Metadata>(fixture.Directory(), "cannot inspect");
    CheckFoliageScanFailure<SpeciesScanFailure::Increment>(fixture.Directory(), "cannot advance");
}

// This virtual provider supplies an unencodable filename without requiring a
// host filesystem that accepts unpaired UTF-16 surrogates. The failure happens
// when the filename is converted to the filesystem path used by the UTF-8 encoder.
template<bool Unencodable = true>
class VirtualSpeciesDirectoryIterator {
public:
    class Filename {
        fs::path path_;
        bool unencodable_;

    public:
        Filename(fs::path path, bool unencodable)
            : path_(std::move(path))
            , unencodable_(unencodable) {}
        operator fs::path() const {
            if (unencodable_)
                throw fs::filesystem_error("unpaired UTF-16 surrogate",
                                           std::make_error_code(std::errc::illegal_byte_sequence));
            return path_;
        }
    };
    class Path {
        fs::path path_;
        bool unencodable_;

    public:
        Path(fs::path path, bool unencodable)
            : path_(std::move(path))
            , unencodable_(unencodable) {}
        fs::path extension() const {
            return path_.extension();
        }
        Filename filename() const {
            return {path_.filename(), unencodable_};
        }
        operator fs::path() const {
            return path_;
        }
    };
    class Entry {
        fs::path directory_;
        std::size_t index_ = 3;

    public:
        Entry() = default;
        Entry(fs::path directory, std::size_t index)
            : directory_(std::move(directory))
            , index_(index) {}
        bool is_regular_file(std::error_code& ec) const {
            ec.clear();
            return true;
        }
        Path path() const {
            constexpr std::array<const char*, 3> names{"a.json", "invalid.json", "z.json"};
            return {directory_ / names.at(index_), Unencodable && index_ == 1};
        }
    };
    VirtualSpeciesDirectoryIterator() = default;
    VirtualSpeciesDirectoryIterator(fs::path directory, std::error_code& ec)
        : directory_(std::move(directory))
        , index_(0) {
        ec.clear();
    }
    Entry operator*() const {
        return {directory_, index_};
    }
    bool operator!=(const VirtualSpeciesDirectoryIterator& other) const {
        return index_ != other.index_;
    }
    void increment(std::error_code& ec) {
        ++index_;
        ec.clear();
    }

private:
    fs::path directory_;
    std::size_t index_ = 3;
};

TEST(GameSessionHeadlessWorldTest, FoliageSkipsUnencodableNamesAndLoadsBothValidNeighbours) {
    const Luminumbra::TestSupport::SpeciesIdentityFixture fixture;
    fixture.Write("a.json", R"({"id":"first"})");
    fixture.Write("invalid.json", R"({"id":"excluded"})");
    fixture.Write("z.json", R"({"id":"last"})");
    luminumbra::foliage::SpeciesRegistry registry;
    std::vector<std::string> errors;
    bool io_failed = true;
    EXPECT_NO_THROW(EXPECT_EQ(registry.LoadFromDirectory<VirtualSpeciesDirectoryIterator<>>(
                                  fixture.Directory(), errors, &io_failed),
                              2u));
    EXPECT_FALSE(io_failed);
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_NE(errors.front().find("filename UTF-8 conversion failed"), std::string::npos);
    EXPECT_NE(errors.front().find("unpaired UTF-16 surrogate"), std::string::npos);
    ASSERT_EQ(registry.size(), 2u);
    EXPECT_EQ(registry.all()[0].id, "first");
    EXPECT_EQ(registry.all()[1].id, "last");
    EXPECT_EQ(registry.Find("excluded"), nullptr);
    // World dependency identity must refuse an incomplete creature roster.
    luminumbra::ai::CreatureSpeciesRegistry creatures;
    errors.clear();
    EXPECT_NO_THROW(
        EXPECT_EQ(creatures.LoadFromDirectory<VirtualSpeciesDirectoryIterator<>>(
                      fixture.Directory(), errors, Luminumbra::World::ReadWorldgenFile, &io_failed),
                  0u));
    EXPECT_TRUE(io_failed);
    EXPECT_EQ(creatures.size(), 0u);
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_NE(errors.front().find("unpaired UTF-16 surrogate"), std::string::npos);
}

TEST(GameSessionHeadlessWorldTest, FoliageFileIoFailureRollsBackBeforeRetry) {
    for (const bool unreadable : {false, true}) {
        const Luminumbra::TestSupport::SpeciesIdentityFixture fixture;
        fixture.Write("a.json", R"({"id":"first"})");
        fixture.Write("z.json", R"({"id":"last"})");
        // A vanished file fails open; a directory accepted by the virtual metadata
        // provider fails either open or read, according to the host stream library.
        if (unreadable)
            fs::create_directory(fixture.Directory() / "invalid.json");
        luminumbra::foliage::SpeciesRegistry registry;
        std::string parse_error;
        ASSERT_TRUE(registry.AddFromJsonText(R"({"id":"retained"})", parse_error));
        std::vector<std::string> errors;
        bool io_failed = false;
        EXPECT_EQ(registry.LoadFromDirectory<VirtualSpeciesDirectoryIterator<false>>(
                      fixture.Directory(), errors, &io_failed),
                  0u);
        EXPECT_TRUE(io_failed);
        ASSERT_EQ(errors.size(), 1u);
        EXPECT_NE(errors.front().find("species file: invalid.json"), std::string::npos);
        ASSERT_EQ(registry.size(), 1u);
        EXPECT_NE(registry.Find("retained"), nullptr);
        if (unreadable)
            fs::remove(fixture.Directory() / "invalid.json");
        fixture.Write("invalid.json", R"({"id":"recovered"})");
        errors.clear();
        EXPECT_EQ(registry.LoadFromDirectory(fixture.Directory(), errors, &io_failed), 4u);
        EXPECT_FALSE(io_failed);
        EXPECT_TRUE(errors.empty());
        EXPECT_EQ(registry.size(), 5u);
    }
}

template<bool StandardException>
class ThrowingSpeciesDirectoryIterator
    : public FailingSpeciesDirectoryIterator<SpeciesScanFailure::Open> {
public:
    ThrowingSpeciesDirectoryIterator() = default;
    ThrowingSpeciesDirectoryIterator(const fs::path&, std::error_code&) {
        if constexpr (StandardException)
            throw std::runtime_error("directory provider failed");
        else
            throw 1;
    }
};

TEST(GameSessionHeadlessWorldTest, FoliageDirectoryProviderExceptionsAreContained) {
    const Luminumbra::TestSupport::SpeciesIdentityFixture fixture;
    luminumbra::foliage::SpeciesRegistry registry;
    std::vector<std::string> errors;
    bool io_failed = false;
    EXPECT_NO_THROW(EXPECT_EQ(registry.LoadFromDirectory<ThrowingSpeciesDirectoryIterator<true>>(
                                  fixture.Directory(), errors, &io_failed),
                              0u));
    EXPECT_TRUE(io_failed);
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_NE(errors.back().find("directory provider failed"), std::string::npos);
    errors.clear();
    EXPECT_NO_THROW(EXPECT_EQ(registry.LoadFromDirectory<ThrowingSpeciesDirectoryIterator<false>>(
                                  fixture.Directory(), errors, &io_failed),
                              0u));
    EXPECT_TRUE(io_failed);
    ASSERT_EQ(errors.size(), 1u);
    EXPECT_NE(errors.back().find("unknown exception"), std::string::npos);
    EXPECT_EQ(registry.size(), 0u);
}

TEST(GameSessionHeadlessWorldTest, FoliageUnavailableAndEmptyDirectoriesHaveDistinctResults) {
    const HeadlessRoot root;
    luminumbra::foliage::SpeciesRegistry registry;
    std::vector<std::string> errors;
    bool io_failed = false;
    for (const auto& unavailable : {root.path() / "missing", SourcePresetPath()}) {
        EXPECT_EQ(registry.LoadFromDirectory(unavailable, errors, &io_failed), 0u);
        EXPECT_TRUE(io_failed);
        ASSERT_FALSE(errors.empty());
        EXPECT_NE(errors.back().find("directory unavailable"), std::string::npos);
    }
    const auto empty = root.path() / "empty";
    fs::create_directory(empty);
    errors.clear();
    EXPECT_EQ(registry.LoadFromDirectory(empty, errors, &io_failed), 0u);
    EXPECT_FALSE(io_failed);
    EXPECT_TRUE(errors.empty());
}

TEST(GameSessionHeadlessWorldTest, FarmingSpeciesRetriesAtDeadlineAndLogsOnlyTransitions) {
    using Cache = Luminumbra::Client::App::FarmSpeciesCache;
    using Result = Cache::UpdateResult;
    const HeadlessRoot root;
    const auto directory = root.path() / "farm-species";
    Cache cache;
    EXPECT_TRUE(cache.NeedsUpdate(10.0));
    EXPECT_EQ(cache.Update(10.0, directory), Result::Diagnostics);
    EXPECT_FALSE(cache.loaded());
    ASSERT_FALSE(cache.diagnostics().empty());
    const auto first_error = cache.diagnostics();
    // An unchanged persistent failure is retried, but does not repeat warnings.
    EXPECT_EQ(cache.Update(15.0, directory), Result::Unchanged);
    EXPECT_EQ(cache.diagnostics(), first_error);
    // A different failure is reported at the next deadline.
    {
        std::ofstream file(directory);
        file << "not a directory";
    }
    EXPECT_FALSE(cache.NeedsUpdate(19.999));
    EXPECT_TRUE(cache.NeedsUpdate(20.0));
    EXPECT_EQ(cache.Update(19.999, directory), Result::Unchanged);
    EXPECT_EQ(cache.Update(20.0, directory), Result::Diagnostics);
    EXPECT_NE(cache.diagnostics(), first_error);
    fs::remove(directory);
    fs::create_directory(directory);
    {
        std::ofstream file(directory / "valid.json");
        file << R"({"id":"wheat"})";
    }
    {
        std::ofstream file(directory / "partial.json");
        file << R"({"id":)";
    }
    // Repair during cooldown cannot cause synchronous loading on waiting frames.
    for (const double time : {20.0, 21.0, 24.999}) {
        EXPECT_EQ(cache.Update(time, directory), Result::Unchanged);
        EXPECT_FALSE(cache.loaded());
        EXPECT_TRUE(cache.all().empty());
    }
    EXPECT_EQ(cache.Update(25.0, directory), Result::Recovered);
    EXPECT_TRUE(cache.loaded());
    ASSERT_EQ(cache.all().size(), 1u);
    EXPECT_EQ(cache.all().front().id, "wheat");
    ASSERT_EQ(cache.diagnostics().size(), 1u);
    EXPECT_NE(cache.diagnostics().front().find("partial.json"), std::string::npos);
    fs::remove_all(directory);
    EXPECT_FALSE(cache.NeedsUpdate(100.0));
    EXPECT_EQ(cache.Update(100.0, directory), Result::Unchanged);
    EXPECT_EQ(cache.all().size(), 1u);
}

TEST(GameSessionHeadlessWorldTest, FarmingSpeciesEmptySuccessIsCachedAndRecoveryIsReportedOnce) {
    using Cache = Luminumbra::Client::App::FarmSpeciesCache;
    using Result = Cache::UpdateResult;
    const HeadlessRoot root;
    const auto directory = root.path() / "empty-farm-species";
    fs::create_directory(directory);
    Cache cache;
    EXPECT_EQ(cache.Update(0.0, directory), Result::Diagnostics);
    EXPECT_TRUE(cache.loaded());
    EXPECT_TRUE(cache.all().empty());
    EXPECT_TRUE(cache.diagnostics().empty());
    fs::remove(directory);
    EXPECT_EQ(cache.Update(10.0, directory), Result::Unchanged);
    Cache recovering;
    EXPECT_EQ(recovering.Update(0.0, directory), Result::Diagnostics);
    fs::create_directory(directory);
    EXPECT_EQ(recovering.Update(5.0, directory), Result::Recovered);
    EXPECT_TRUE(recovering.loaded());
    EXPECT_TRUE(recovering.diagnostics().empty());
    EXPECT_EQ(recovering.Update(10.0, directory), Result::Unchanged);
}

TEST(GameSessionHeadlessWorldTest, SpeciesScanFailuresAreContainedBeforeAnyReads) {
    const Luminumbra::TestSupport::SpeciesIdentityFixture fixture;
    CheckSpeciesScanFailure<SpeciesScanFailure::Open>(fixture.Directory(), "cannot enumerate");
    CheckSpeciesScanFailure<SpeciesScanFailure::Metadata>(fixture.Directory(), "cannot inspect");
    CheckSpeciesScanFailure<SpeciesScanFailure::Increment>(fixture.Directory(), "cannot advance");
}

TEST(GameSessionHeadlessWorldTest, SpeciesEnumerationFailurePublishesNoIdentity) {
    using Luminumbra::TestSupport::SpeciesIdentityFixture;
    const SpeciesIdentityFixture fixture;
    ASSERT_TRUE(fixture.Resolve().ok);
    unsigned species_status_queries = 0;
    const Luminumbra::World::WorldgenFileStatusReader remove_after_status =
        [&](const fs::path& path, std::error_code& ec) {
            const auto status = fs::symlink_status(path, ec);
            if (path == fixture.Directory() && ++species_status_queries == 2) {
                // The registry has valid directory metadata, but its subsequent
                // iterator open must fail. This works without POSIX permission bits.
                EXPECT_FALSE(ec);
                EXPECT_TRUE(fs::is_directory(status));
                fs::remove_all(path);
            }
            return status;
        };
    const auto refused = Luminumbra::world::LoadTerrainPresetFromJson(
        SpeciesIdentityFixture::Preset(), fixture.Root() / "data", "<memory>", remove_after_status);
    EXPECT_EQ(species_status_queries, 2u);
    EXPECT_FALSE(refused.ok);
    EXPECT_TRUE(refused.preset_content_hash.empty());
    EXPECT_EQ(refused.params.worldgen_dependencies, nullptr);
    ASSERT_EQ(refused.errors.size(), 2u);
    EXPECT_NE(refused.errors.front().find("species dependencies"), std::string::npos);
    EXPECT_NE(refused.errors.back().find("cannot enumerate creature species directory"),
              std::string::npos);
}

TEST(GameSessionHeadlessWorldTest, SpeciesReaderExceptionsAreContainedAndRolledBack) {
    const Luminumbra::TestSupport::SpeciesIdentityFixture fixture;
    fixture.Write("a.json", R"({"id":"discarded"})");
    for (const bool standard_exception : {true, false}) {
        luminumbra::ai::CreatureSpeciesRegistry registry;
        std::string parse_error;
        ASSERT_TRUE(registry.AddFromJsonText(R"({"id":"retained"})", parse_error));
        std::vector<std::string> errors;
        bool io_failed = false;
        unsigned reads = 0;
        const Luminumbra::World::WorldgenFileReader throw_read = [&](const fs::path& path,
                                                                     std::string& bytes) {
            if (++reads == 2) {
                if (standard_exception)
                    throw fs::filesystem_error(
                        "read failure", path, std::make_error_code(std::errc::io_error));
                throw 1;
            }
            return Luminumbra::World::ReadWorldgenFile(path, bytes);
        };
        std::size_t loaded = 1;
        EXPECT_NO_THROW(loaded = registry.LoadFromDirectory(
                            fixture.Directory(), errors, throw_read, &io_failed));
        EXPECT_EQ(loaded, 0u);
        EXPECT_TRUE(io_failed);
        EXPECT_EQ(reads, 2u);
        ASSERT_EQ(errors.size(), 1u);
        EXPECT_NE(errors.front().find("directory load failed"), std::string::npos);
        ASSERT_EQ(registry.size(), 1u);
        EXPECT_NE(registry.FindByName("retained"), nullptr);
        EXPECT_EQ(registry.FindByName("discarded"), nullptr);
    }
}

TEST(GameSessionHeadlessWorldTest, DependencyPathUtf8ValidationIsBoundedAndPortable) {
    using Luminumbra::World::IsValidDependencyPathUtf8;
    // Deliberately not null-terminated: even a one-byte input must stay in bounds.
    const std::array<char, 1> single_byte{'0'};
    EXPECT_TRUE(
        IsValidDependencyPathUtf8(std::string_view(single_byte.data(), single_byte.size())));
    const std::array<char, 1> truncated_lead{'\xc2'};
    EXPECT_FALSE(
        IsValidDependencyPathUtf8(std::string_view(truncated_lead.data(), truncated_lead.size())));
    for (const std::string_view valid : {"",
                                         "common/creatures/species/deer.json",
                                         "\x7f",
                                         "\xc2\x80",
                                         "\xdf\xbf",
                                         "\xe0\xa0\x80",
                                         "\xed\x9f\xbf",
                                         "\xee\x80\x80",
                                         "\xef\xbf\xbf",
                                         "\xf0\x90\x80\x80",
                                         "\xf4\x8f\xbf\xbf",
                                         "e\xcc\x81.json"}) {
        EXPECT_TRUE(IsValidDependencyPathUtf8(valid));
    }
    for (const std::string_view invalid : {"\xff.json",
                                           "\x80",
                                           "\xbf",
                                           "\xc0\x80",
                                           "\xc1\xbf",
                                           "\xc2",
                                           "\xdf",
                                           "\xe0",
                                           "\xe0\xa0",
                                           "\xf0",
                                           "\xf0\x90",
                                           "\xf0\x90\x80",
                                           "\xc2/",
                                           "\xe0\xa0/",
                                           "\xf0\x90\x80/",
                                           "\xe0\x9f\xbf",
                                           "\xed\xa0\x80",
                                           "\xed\xbf\xbf",
                                           "\xf0\x8f\xbf\xbf",
                                           "\xf4\x90\x80\x80",
                                           "\xf5\x80\x80\x80",
                                           "\xfe"}) {
        EXPECT_FALSE(IsValidDependencyPathUtf8(invalid));
        EXPECT_FALSE(IsValidDependencyPathUtf8(std::string("common/creatures/species/") +
                                               std::string(invalid)));
    }
}

TEST(GameSessionHeadlessWorldTest, PresetSerializationFailuresPublishNoIdentity) {
    using Luminumbra::TestSupport::SpeciesIdentityFixture;
    const SpeciesIdentityFixture fixture;
    for (const bool invalid_key : {false, true}) {
        auto preset = SpeciesIdentityFixture::Preset();
        if (invalid_key)
            preset["\xff"] = "unknown field";
        else
            preset["description"] = "\xff";
        Luminumbra::world::TerrainPresetLoadResult refused;
        EXPECT_NO_THROW(refused = Luminumbra::world::LoadTerrainPresetFromJson(
                            preset, fixture.Root() / "data"));
        EXPECT_FALSE(refused.ok);
        EXPECT_TRUE(refused.preset_content_hash.empty());
        EXPECT_EQ(refused.params.worldgen_dependencies, nullptr);
        ASSERT_FALSE(refused.errors.empty());
        EXPECT_NE(refused.errors.back().find("failed to resolve world preset"), std::string::npos);
    }
}

TEST(GameSessionHeadlessWorldTest, InvalidLogicalDependencyPathPublishesNoIdentity) {
    using Luminumbra::TestSupport::SpeciesIdentityFixture;
    const SpeciesIdentityFixture fixture;
    auto preset = SpeciesIdentityFixture::Preset();
    preset["generation_params"]["biomes"]["table"] = "common/\xff.json";
    Luminumbra::world::TerrainPresetLoadResult refused;
    EXPECT_NO_THROW(
        refused = Luminumbra::world::LoadTerrainPresetFromJson(preset, fixture.Root() / "data"));
    EXPECT_FALSE(refused.ok);
    EXPECT_TRUE(refused.preset_content_hash.empty());
    EXPECT_EQ(refused.params.worldgen_dependencies, nullptr);
    EXPECT_FALSE(refused.errors.empty());
}

TEST(GameSessionHeadlessWorldTest, ResolverExceptionsPublishNoIdentity) {
    using Luminumbra::TestSupport::SpeciesIdentityFixture;
    const SpeciesIdentityFixture fixture;
    for (const bool standard_exception : {true, false}) {
        const Luminumbra::World::WorldgenFileStatusReader throw_status =
            [standard_exception](const fs::path&, std::error_code&) -> fs::file_status {
            if (standard_exception)
                throw std::runtime_error("dependency status failed");
            throw 1;
        };
        Luminumbra::world::TerrainPresetLoadResult refused;
        EXPECT_NO_THROW(
            refused = Luminumbra::world::LoadTerrainPresetFromJson(SpeciesIdentityFixture::Preset(),
                                                                   fixture.Root() / "data",
                                                                   "<memory>",
                                                                   throw_status));
        EXPECT_FALSE(refused.ok);
        EXPECT_TRUE(refused.preset_content_hash.empty());
        EXPECT_EQ(refused.params.worldgen_dependencies, nullptr);
        ASSERT_EQ(refused.errors.size(), 1u);
        EXPECT_NE(refused.errors.front().find(standard_exception ? "dependency status failed"
                                                                 : "unknown exception"),
                  std::string::npos);
    }
}

#ifndef _WIN32
// Native raw-byte filenames are POSIX-only; the validation and resolver seams above
// exercise the same refusals portably without constructing an invalid native path.
TEST(GameSessionHeadlessWorldTest, InvalidSpeciesFilenameRefusesBeforeRegistryAndWorldAdoption) {
    using Luminumbra::TestSupport::SpeciesIdentityFixture;
    const SpeciesIdentityFixture fixture;
    const auto baseline = fixture.Resolve();
    ASSERT_TRUE(baseline.ok);
    JobSystem jobs;
    jobs.startup(1);
    {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(Luminumbra::Filesystem::GenericUtf8PathString(fixture.Root()));
        ASSERT_TRUE(session.CreateWorld("Retained world", "777", "default"));
        const auto world_id = session.GetMetadata().worldId;
        const auto* world = session.GetWorldSystem();
        const auto* table = session.GetSpeciesTable();
        const auto tick = session.GetSimulationTickCount();
        const auto identity = session.PresetContentHash();
        fixture.Write(fs::path("\xff.json"), R"({"id":"invalid_filename"})");

        luminumbra::ai::CreatureSpeciesRegistry registry;
        std::string parse_error;
        ASSERT_TRUE(registry.AddFromJsonText(R"({"id":"retained"})", parse_error));
        std::vector<std::string> errors;
        bool io_failed = false;
        unsigned reads = 0;
        const Luminumbra::World::WorldgenFileReader reader = [&](const fs::path& file,
                                                                 std::string& bytes) {
            ++reads;
            return Luminumbra::World::ReadWorldgenFile(file, bytes);
        };
        std::size_t count = 1;
        EXPECT_NO_THROW(
            count = registry.LoadFromDirectory(fixture.Directory(), errors, reader, &io_failed));
        EXPECT_EQ(count, 0u);
        EXPECT_TRUE(io_failed);
        EXPECT_EQ(reads, 0u);
        ASSERT_EQ(registry.size(), 1u);
        EXPECT_NE(registry.FindByName("retained"), nullptr);
        ASSERT_FALSE(errors.empty());
        EXPECT_NE(errors.front().find("not valid UTF-8"), std::string::npos);

        luminumbra::foliage::SpeciesRegistry foliage;
        ASSERT_TRUE(foliage.AddFromJsonText(R"({"id":"retained"})", parse_error));
        errors.clear();
        EXPECT_EQ(foliage.LoadFromDirectory(fixture.Directory(), errors), 1u);
        ASSERT_EQ(foliage.size(), 2u);
        EXPECT_NE(foliage.Find("deer"), nullptr);
        EXPECT_EQ(foliage.Find("invalid_filename"), nullptr);
        EXPECT_NE(foliage.Find("retained"), nullptr);
        ASSERT_EQ(errors.size(), 1u);
        EXPECT_NE(errors.front().find("not valid UTF-8"), std::string::npos);

        Luminumbra::world::TerrainPresetLoadResult refused;
        EXPECT_NO_THROW(refused = fixture.Resolve());
        EXPECT_FALSE(refused.ok);
        EXPECT_TRUE(refused.preset_content_hash.empty());
        EXPECT_EQ(refused.params.worldgen_dependencies, nullptr);
        ASSERT_FALSE(refused.errors.empty());
        EXPECT_NE(refused.errors.back().find("not valid UTF-8"), std::string::npos);
        const auto expect_unchanged = [&] {
            EXPECT_EQ(session.GetWorldSystem(), world);
            EXPECT_EQ(session.GetSpeciesTable(), table);
            EXPECT_EQ(session.PresetContentHash(), identity);
            EXPECT_EQ(session.GetMetadata().worldId, world_id);
            EXPECT_EQ(session.GetSimulationTickCount(), tick);
            EXPECT_NE(session.GetWorldOpenError().find("species"), std::string::npos);
        };
        bool opened = true;
        EXPECT_NO_THROW(opened = session.CreateWorld("Refused world", "888", "default"));
        EXPECT_FALSE(opened);
        expect_unchanged();
        EXPECT_NO_THROW(opened = session.LoadWorld(world_id));
        EXPECT_FALSE(opened);
        expect_unchanged();
        GameSession fresh;
        fresh.SetJobSystem(&jobs);
        fresh.SetRootPath(Luminumbra::Filesystem::GenericUtf8PathString(fixture.Root()));
        EXPECT_NO_THROW(opened = fresh.CreateWorld("Refused fresh world", "999", "default"));
        EXPECT_FALSE(opened);
        EXPECT_TRUE(fresh.PresetContentHash().empty());
        EXPECT_EQ(fresh.GetWorldSystem(), nullptr);
        EXPECT_EQ(fresh.GetSpeciesTable(), nullptr);
        fs::remove(fixture.Directory() / fs::path("\xff.json"));
        const auto recovered = fixture.Resolve();
        ASSERT_TRUE(recovered.ok);
        EXPECT_EQ(recovered.preset_content_hash, baseline.preset_content_hash);
    }
    jobs.shutdown();
}
#endif

TEST(GameSessionHeadlessWorldTest, FoliageSpeciesUsePortableFilenameOrderForDuplicateIds) {
    const HeadlessRoot root;
    // U+E000 precedes U+10000 in UTF-8, but follows it in native UTF-16 order.
    const fs::path earlier(u8"\ue000.json"), later(u8"\U00010000.json");
    for (const bool reverse : {false, true}) {
        const auto directory = root.path() / (reverse ? "reverse" : "forward");
        fs::create_directories(directory);
        luminumbra::foliage::SpeciesRegistry registry;
        std::vector<std::string> errors;
        EXPECT_EQ(registry.LoadFromDirectory(directory, errors), 0u);
        EXPECT_TRUE(errors.empty());
        const auto write = [&](const fs::path& name, const std::string& bytes) {
            std::ofstream output(directory / name, std::ios::binary);
            output << bytes;
            output.close();
            ASSERT_TRUE(output.good());
        };
        ASSERT_NO_FATAL_FAILURE(write(reverse ? later : earlier,
                                      reverse
                                          ? R"({"id":"duplicate","render_archetype":"later"})"
                                          : R"({"id":"duplicate","render_archetype":"earlier"})"));
        ASSERT_NO_FATAL_FAILURE(write(reverse ? earlier : later,
                                      reverse
                                          ? R"({"id":"duplicate","render_archetype":"earlier"})"
                                          : R"({"id":"duplicate","render_archetype":"later"})"));
        ASSERT_NO_FATAL_FAILURE(write("malformed.json", "{"));
        ASSERT_NO_FATAL_FAILURE(write("ignored.txt", "not JSON"));
        EXPECT_EQ(registry.LoadFromDirectory(directory, errors), 2u);
        ASSERT_EQ(errors.size(), 1u);
        EXPECT_NE(errors.front().find("malformed.json"), std::string::npos);
        ASSERT_EQ(registry.all().size(), 2u);
        EXPECT_EQ(registry.all()[0].render_archetype, "earlier");
        EXPECT_EQ(registry.all()[1].render_archetype, "later");
        const auto* selected = registry.Find("duplicate");
        ASSERT_NE(selected, nullptr);
        EXPECT_EQ(selected->render_archetype, "earlier");
    }
}

TEST(GameSessionHeadlessWorldTest, SpeciesSnapshotBindsExactBytesAndPortableOrder) {
    using Luminumbra::TestSupport::SpeciesIdentityFixture;
    const SpeciesIdentityFixture first, second;
    // UTF-8 order differs from UTF-16 native order for these two filenames.
    const fs::path earlier(u8"\ue000.json"), later(u8"\U00010000.json");
    const std::string earlier_bytes = R"({"id":"first","brain":{"wander_weight":0.4}})";
    const std::string later_bytes = R"({"id":"last","brain":{"wander_weight":0.7}})";
    first.Write(later, later_bytes);
    first.Write(earlier, earlier_bytes);
    second.Write(earlier, earlier_bytes);
    second.Write(later, later_bytes);
    first.Write("nested/ignored.json", R"({"id":"ignored"})");
    first.Write("ignored.txt", "not a species input");
    const auto a = first.Resolve(), b = second.Resolve();
    ASSERT_TRUE(a.ok);
    ASSERT_TRUE(b.ok);
    EXPECT_EQ(a.preset_content_hash, b.preset_content_hash);
    const auto snapshot = a.params.worldgen_dependencies;
    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(snapshot->manifest, b.params.worldgen_dependencies->manifest);
    ASSERT_EQ(snapshot->manifest.size(), 4u);
    // Exact-byte FNV-1a-64 of SpeciesIdentityFixture::OriginalBytes(), using
    // the manifest's offset basis 14695981039346656037.
    EXPECT_EQ(snapshot->manifest[1],
              (std::vector<std::string>{"common/creatures/species/deer.json", "f730450646ea14a9"}));
    EXPECT_EQ(snapshot->manifest[2][0],
              "common/creatures/species/" + Luminumbra::Filesystem::GenericUtf8PathString(earlier));
    EXPECT_EQ(snapshot->manifest[3][0],
              "common/creatures/species/" + Luminumbra::Filesystem::GenericUtf8PathString(later));
    ASSERT_NE(snapshot->species_table, nullptr);
    ASSERT_EQ(snapshot->species_table->size(), 3u);
    EXPECT_EQ(snapshot->species_table->all()[1].id, "first");
    EXPECT_EQ(snapshot->species_table->all()[2].id, "last");
    // Reopening changed bytes changes identity, but never the captured registry.
    first.Write("deer.json", R"({"id":"deer","brain":{"wander_weight":0.9}})");
    const auto changed = first.Resolve();
    ASSERT_TRUE(changed.ok);
    EXPECT_NE(changed.preset_content_hash, a.preset_content_hash);
    const auto* changed_deer =
        changed.params.worldgen_dependencies->species_table->FindByName("deer");
    ASSERT_NE(changed_deer, nullptr);
    EXPECT_FLOAT_EQ(changed_deer->brain.wander_weight, 0.9f);
    const auto* deer = snapshot->species_table->FindByName("deer");
    ASSERT_NE(deer, nullptr);
    EXPECT_FLOAT_EQ(deer->brain.wander_weight, 0.25f);
    EXPECT_FLOAT_EQ(deer->genome_ranges.core[0].lo, 1.0f);
    EXPECT_FLOAT_EQ(deer->genome_ranges.core[0].hi, 2.0f);
}

TEST(GameSessionHeadlessWorldTest, SpeciesChangesRefuseLockstepAndReplayBeforeTicks) {
    namespace net = Luminumbra::Net;
    namespace replay = Luminumbra::Replay;
    using Luminumbra::TestSupport::SpeciesIdentityFixture;
    const SpeciesIdentityFixture fixture;
    const auto baseline = fixture.Resolve();
    ASSERT_TRUE(baseline.ok);
    const auto& content_hash = baseline.preset_content_hash;
    std::vector<std::pair<std::string, std::string>> variants;
    const auto capture = [&](const std::string& name) {
        const auto resolved = fixture.Resolve();
        EXPECT_TRUE(resolved.ok);
        EXPECT_NE(resolved.preset_content_hash, content_hash) << name;
        variants.emplace_back(name, resolved.preset_content_hash);
    };
    auto species = nlohmann::json::parse(SpeciesIdentityFixture::OriginalBytes());
    species["brain"]["wander_weight"] = 0.9;
    fixture.Write("deer.json", species.dump());
    capture("brain override");
    species = nlohmann::json::parse(SpeciesIdentityFixture::OriginalBytes());
    species["genome_ranges"]["move_speed"] = {2, 3};
    fixture.Write("deer.json", species.dump());
    capture("genome bounds");
    fixture.Write("deer.json", SpeciesIdentityFixture::OriginalBytes() + "\n");
    capture("exact bytes including whitespace");
    fixture.Write("deer.json", "{invalid");
    capture("rejected JSON bytes");
    const auto rejected = fixture.Resolve();
    ASSERT_TRUE(rejected.ok);
    EXPECT_FALSE(rejected.warnings.empty());
    EXPECT_EQ(rejected.params.worldgen_dependencies->species_table->size(), 0u);
    fixture.Write("deer.json", SpeciesIdentityFixture::OriginalBytes());
    fs::rename(fixture.Directory() / "deer.json", fixture.Directory() / "renamed.json");
    capture("logical path");
    fs::rename(fixture.Directory() / "renamed.json", fixture.Directory() / "deer.json");
    fixture.Write("added.json", R"({"id":"added"})");
    capture("added species");
    fs::remove(fixture.Directory() / "added.json");
    fs::remove(fixture.Directory() / "deer.json");
    capture("removed species, empty directory");
    fs::remove(fixture.Directory());
    capture("absent species directory, older preset-only identity");
    EXPECT_EQ(variants.back().second,
              Luminumbra::world::TerrainPresetContentHash(SpeciesIdentityFixture::Preset()));

    replay::ReplayHeader header;
    header.seed = 424242;
    header.preset = "default";
    header.system_config_identity = GameSession{}.SystemConfigIdentity();
    header.preset_content_hash = content_hash;
    const Luminumbra::TestSupport::ScopedTempFile stream("species-replay-", ".lrec1");
    const auto path = Luminumbra::Filesystem::GenericUtf8PathString(stream.Path());
    replay::ReplayWriter writer;
    ASSERT_TRUE(writer.Open(path, header));
    ASSERT_TRUE(writer.Finalize(0));
    const auto contents = replay::ReadReplay(path);
    ASSERT_TRUE(contents.has_value());
    if (!contents.has_value())
        return;
    ASSERT_FALSE(contents->truncated);
    ASSERT_TRUE(contents->trailer_present);
    EXPECT_EQ(contents->header.preset_content_hash, content_hash);
    variants.emplace_back("identical species", content_hash);
    for (const auto& [name, peer_hash] : variants) {
        SCOPED_TRACE(name);
        const bool equal = name == "identical species";
        EXPECT_EQ(replay::ConfigurationMatches(
                      contents->header, header.system_config_identity, peer_hash),
                  equal);
        auto reverse = contents->header;
        reverse.preset_content_hash = peer_hash;
        EXPECT_EQ(
            replay::ConfigurationMatches(reverse, header.system_config_identity, content_hash),
            equal);

        auto [host_transport, peer_transport] = net::MakeLoopbackPair();
        net::LockstepConfig config;
        config.seed = header.seed;
        config.preset = header.preset;
        config.system_config_identity = header.system_config_identity;
        config.preset_content_hash = content_hash;
        net::HelloMsg hello;
        hello.seed = config.seed;
        hello.preset = config.preset;
        hello.client_id = 1;
        hello.system_config_identity = config.system_config_identity;
        hello.preset_content_hash = peer_hash;
        ASSERT_TRUE(peer_transport->SendFrame(net::EncodeHello(hello)));
        std::uint64_t ticks = 0;
        net::LockstepHooks hooks;
        hooks.user = &ticks;
        hooks.apply_and_step = [](std::uint64_t, const std::vector<std::uint8_t>&, void* user) {
            ++*static_cast<std::uint64_t*>(user);
            return true;
        };
        net::LockstepSession host(config, host_transport.get(), hooks);
        EXPECT_EQ(host.Handshake(), equal);
        EXPECT_EQ(host.Status().handshaken, equal);
        EXPECT_EQ(ticks, 0u);
    }
}

TEST(GameSessionHeadlessWorldTest, SpeciesIdentityIsAdoptedOnCreateLoadAndIoRefusal) {
    using Luminumbra::TestSupport::SpeciesIdentityFixture;
    const SpeciesIdentityFixture fixture;
    const auto baseline = Luminumbra::world::LoadTerrainPresetFromJson(
        Luminumbra::world::ResolveEffectivePreset(SpeciesIdentityFixture::Preset(),
                                                  Luminumbra::world::TerrainPresetUse::NewWorld),
        fixture.Root() / "data");
    ASSERT_TRUE(baseline.ok);
    // Reader failure coverage is independent of OS privilege/permission bits.
    luminumbra::ai::CreatureSpeciesRegistry incomplete;
    std::vector<std::string> errors;
    bool io_failed = false;
    const Luminumbra::World::WorldgenFileReader fail_read = [](const fs::path&, std::string&) {
        return false;
    };
    EXPECT_EQ(incomplete.LoadFromDirectory(fixture.Directory(), errors, fail_read, &io_failed), 0u);
    EXPECT_TRUE(io_failed);
    EXPECT_FALSE(errors.empty());
    JobSystem jobs;
    jobs.startup(1);
    {
        GameSession created, loaded;
        for (auto* session : {&created, &loaded}) {
            session->SetJobSystem(&jobs);
            session->SetRootPath(Luminumbra::Filesystem::GenericUtf8PathString(fixture.Root()));
        }
        ASSERT_TRUE(created.CreateWorld("Species identity", "777", "default"));
        EXPECT_EQ(created.PresetContentHash(), baseline.preset_content_hash);
        const auto id = created.GetMetadata().worldId;
        ASSERT_TRUE(loaded.LoadWorld(id));
        EXPECT_EQ(loaded.PresetContentHash(), created.PresetContentHash());
        ASSERT_NE(created.GetSpeciesTable(), nullptr);
        const auto* deer = created.GetSpeciesTable()->FindByName("deer");
        ASSERT_NE(deer, nullptr);
        EXPECT_FLOAT_EQ(deer->brain.wander_weight, 0.25f);
        fixture.Write("deer.json", R"({"id":"deer","brain":{"wander_weight":0.9}})");
        ASSERT_TRUE(loaded.LoadWorld(id));
        EXPECT_NE(loaded.PresetContentHash(), created.PresetContentHash());
        EXPECT_EQ(created.PresetContentHash(), baseline.preset_content_hash);
        EXPECT_FLOAT_EQ(deer->brain.wander_weight, 0.25f);
        // Saves adopt current content, like preset dependencies: no content hash
        // is persisted in world metadata. A present unreadable input refuses
        // before replacing the live world or publishing another identity.
        const auto adopted = loaded.PresetContentHash();
        const auto* world = loaded.GetWorldSystem();
        const auto* table = loaded.GetSpeciesTable();
        fs::remove_all(fixture.Directory());
        {
            std::ofstream blocker(fixture.Directory(), std::ios::binary);
            blocker << "not a directory";
        }
        const auto refused = fixture.Resolve();
        EXPECT_FALSE(refused.ok);
        EXPECT_TRUE(refused.preset_content_hash.empty());
        ASSERT_FALSE(refused.errors.empty());
        EXPECT_NE(refused.errors.front().find("species"), std::string::npos);
        EXPECT_FALSE(loaded.LoadWorld(id));
        EXPECT_NE(loaded.GetWorldOpenError().find("species"), std::string::npos);
        EXPECT_EQ(loaded.GetWorldSystem(), world);
        EXPECT_EQ(loaded.GetSpeciesTable(), table);
        EXPECT_EQ(loaded.PresetContentHash(), adopted);
    }
    jobs.shutdown();
}

TEST(GameSessionHeadlessWorldTest, ValidateWorldConfigIsSimulationOnlyByDefault) {
    const HeadlessRoot root;
    ASSERT_FALSE(fs::exists(root.path() / "res"));
    ASSERT_FALSE(fs::exists(root.path() / "data/ui"));

    const WorldConfigValidationResult result =
        GameSession::ValidateWorldConfig(root.root_string(), "default");
    EXPECT_TRUE(result.ok) << (result.errors.empty() ? "" : result.errors.front());
    EXPECT_TRUE(result.errors.empty());

    const auto full_data = Luminumbra::world::LoadTerrainPreset(result.preset_path);
    ASSERT_TRUE(full_data.ok);
    ASSERT_GT(fs::remove_all(root.path() / "data"), 0u);
    const auto preset_only = Luminumbra::world::LoadTerrainPreset(result.preset_path);
    EXPECT_FALSE(preset_only.ok);
    EXPECT_FALSE(preset_only.errors.empty());
    EXPECT_TRUE(preset_only.preset_content_hash.empty());
    EXPECT_FALSE(GameSession::ValidateWorldConfig(root.root_string(), "default").ok);
}

TEST(GameSessionHeadlessWorldTest, CallerSuppliedRequiredAssetsAreEnforced) {
    const HeadlessRoot root;

    const std::vector<fs::path> client_assets = {
        fs::path("res") / "shaders" / "basic.vert",
        fs::path("data") / "ui" / "main_menu.rml",
    };
    const WorldConfigValidationResult result =
        GameSession::ValidateWorldConfig(root.root_string(), "default", client_assets);
    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.errors.size(), 2u);
    EXPECT_NE(result.errors.front().find("missing required runtime asset"), std::string::npos);
}

TEST(GameSessionHeadlessWorldTest, SessionRequiredAssetListFailsCreateWorldWhenMissing) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup();
    {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(root.root_string());
        session.SetRequiredClientAssets({fs::path("res") / "shaders" / "basic.vert"});
        EXPECT_FALSE(session.CreateWorld("ClientAssetMissing", "12345", "default"));
    }
    jobs.shutdown();
}

void ExpectRuntimePresetIdentity(const GameSession& session, const fs::path& runtime_root) {
    const auto saved = session.GetWorldSaveDir() / "preset.json";
    const auto expected = Luminumbra::world::LoadTerrainPreset(saved, runtime_root / "data");
    const auto without_runtime = Luminumbra::world::LoadTerrainPreset(saved);
    ASSERT_TRUE(expected.ok);
    ASSERT_FALSE(without_runtime.ok);
    EXPECT_FALSE(without_runtime.errors.empty());
    EXPECT_TRUE(without_runtime.preset_content_hash.empty());
    EXPECT_EQ(session.PresetContentHash(), expected.preset_content_hash);
}

TEST(GameSessionHeadlessWorldTest, SaveRootOverrideRoutesCreateLoadSaveAndCatalog) {
    const HeadlessRoot assets;
    const HeadlessRoot storage;
    ASSERT_GT(fs::remove_all(storage.path() / "data"), 0u);
    ASSERT_GT(fs::remove_all(storage.path() / "worlds/atlas"), 0u);
    JobSystem jobs;
    jobs.startup(1);
    std::string world_id;
    std::string preset_identity;
    std::uint64_t content_identity = 0;
    {
        GameSession baseline;
        baseline.SetJobSystem(&jobs);
        baseline.SetRootPath(assets.root_string());
        ASSERT_TRUE(baseline.CreateTransientWorld("DefaultLocation", "12345", "default"));
        preset_identity = baseline.PresetContentHash();
        content_identity = Luminumbra::World::ComputeTerrainParamsHash(
            baseline.GetWorldSystem()->get_params(), baseline.GetWorldSystem()->get_seed());
        ASSERT_FALSE(preset_identity.empty());
    }
    {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(assets.root_string());
        EXPECT_EQ(session.GetSaveRootPath(), fs::path(assets.root_string()));
        session.SetSaveRootPath(storage.path());
        EXPECT_EQ(session.GetSaveRootPath(), storage.path());
        EXPECT_TRUE(session.GetWorldSaveDir().empty());
        ASSERT_TRUE(session.CreateWorld("PrivateWorld", "12345", "default"));
        EXPECT_EQ(session.PresetContentHash(), preset_identity);
        EXPECT_EQ(Luminumbra::World::ComputeTerrainParamsHash(
                      session.GetWorldSystem()->get_params(), session.GetWorldSystem()->get_seed()),
                  content_identity);
        EXPECT_TRUE(session.GetWorldSystem()->biomes_enabled());
        EXPECT_TRUE(session.GetWorldSystem()->structures_enabled());
        ExpectRuntimePresetIdentity(session, assets.path());
        world_id = session.GetMetadata().worldId;
        const auto expected = storage.path() / "worlds/saves" / world_id;
        EXPECT_EQ(session.GetWorldSaveDir(), expected);
        EXPECT_TRUE(fs::is_regular_file(expected / "preset.json"));
        ASSERT_TRUE(session.SaveWorld());
        ASSERT_TRUE(session.SaveWorldState());
        EXPECT_TRUE(fs::is_regular_file(expected / "world_info.json"));
        const auto catalog =
            Luminumbra::Persistence::EnumerateSavedWorlds(session.GetSaveRootPath(), assets.path());
        EXPECT_TRUE(catalog.error.empty());
        ASSERT_EQ(catalog.worlds.size(), 1u);
        EXPECT_EQ(catalog.worlds.front().metadata.worldId, world_id);
        EXPECT_TRUE(catalog.worlds.front().error.empty()) << catalog.worlds.front().error;
        EXPECT_FALSE(fs::exists(assets.path() / "worlds/saves"));
    }
    {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(assets.root_string());
        session.SetSaveRootPath(storage.path());
        ASSERT_TRUE(session.LoadWorld(world_id)) << session.GetWorldOpenError();
        EXPECT_EQ(session.PresetContentHash(), preset_identity);
        EXPECT_EQ(Luminumbra::World::ComputeTerrainParamsHash(
                      session.GetWorldSystem()->get_params(), session.GetWorldSystem()->get_seed()),
                  content_identity);
        EXPECT_TRUE(session.GetWorldSystem()->biomes_enabled());
        EXPECT_TRUE(session.GetWorldSystem()->structures_enabled());
        ExpectRuntimePresetIdentity(session, assets.path());
        EXPECT_EQ(session.GetMetadata().name, "PrivateWorld");
        ASSERT_TRUE(session.SaveWorld());
        EXPECT_FALSE(fs::exists(assets.path() / "worlds/saves"));
    }
    jobs.shutdown();
}

TEST(GameSessionHeadlessWorldTest, ExistingWorldReloadUsesRuntimeDataWithSeparateStorage) {
    const HeadlessRoot assets;
    const HeadlessRoot storage;
    ASSERT_GT(fs::remove_all(storage.path() / "data"), 0u);
    JobSystem jobs;
    jobs.startup(1);
    std::string world_id;
    std::string preset_identity;
    std::uint64_t content_identity = 0;
    {
        GameSession baseline;
        baseline.SetJobSystem(&jobs);
        baseline.SetRootPath(assets.root_string());
        ASSERT_TRUE(baseline.CreateWorld("ExistingWorld", "12345", "default"));
        ASSERT_TRUE(baseline.SaveWorldState());
        world_id = baseline.GetMetadata().worldId;
        preset_identity = baseline.PresetContentHash();
        content_identity = Luminumbra::World::ComputeTerrainParamsHash(
            baseline.GetWorldSystem()->get_params(), baseline.GetWorldSystem()->get_seed());
    }
    const auto destination = storage.path() / "worlds/saves" / world_id;
    fs::create_directories(destination.parent_path());
    fs::copy(assets.path() / "worlds/saves" / world_id, destination, fs::copy_options::recursive);
    {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(assets.root_string());
        session.SetSaveRootPath(storage.path());
        ASSERT_TRUE(session.LoadWorld(world_id)) << session.GetWorldOpenError();
        EXPECT_EQ(session.PresetContentHash(), preset_identity);
        EXPECT_EQ(Luminumbra::World::ComputeTerrainParamsHash(
                      session.GetWorldSystem()->get_params(), session.GetWorldSystem()->get_seed()),
                  content_identity);
    }
    jobs.shutdown();
}

TEST(GameSessionHeadlessWorldTest, MissingRuntimeDependenciesRefuseCreateReloadAndCatalog) {
    const HeadlessRoot assets;
    const HeadlessRoot storage;
    JobSystem jobs;
    jobs.startup(1);
    std::string world_id;
    {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(assets.root_string());
        session.SetSaveRootPath(storage.path());
        ASSERT_TRUE(session.CreateWorld("DependencyRefusal", "12345", "default"));
        world_id = session.GetMetadata().worldId;
        ASSERT_TRUE(session.SaveWorldState());
    }
    // Valid data beside the saves must never mask missing runtime dependencies.
    // An absent biome table supports legacy classification; a present invalid
    // table and absent/empty structure pools are errors.
    for (const auto& [relative, empty] :
         {std::pair{fs::path("common/biomes.json"), true},
          std::pair{fs::path("common/structures"), false},
          std::pair{fs::path("common/structures"), true},
          std::pair{fs::path("common/structures/cairn/cairn.json"), false},
          std::pair{fs::path("common/structures/cairn/cairn.json"), true}}) {
        SCOPED_TRACE(relative.string());
        SCOPED_TRACE(empty);
        const auto dependency = assets.path() / "data" / relative;
        const auto hidden = assets.path() / "hidden-dependency";
        fs::rename(dependency, hidden);
        if (empty) {
            if (fs::is_directory(hidden))
                fs::create_directory(dependency);
            else
                std::ofstream(dependency, std::ios::binary) << "{}";
        }
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(assets.root_string());
        session.SetSaveRootPath(storage.path());
        EXPECT_FALSE(session.CreateWorld("Refused", "12345", "default"));
        EXPECT_FALSE(session.GetWorldOpenError().empty());
        EXPECT_FALSE(session.CreateTransientWorld("Refused", "12345", "default"));
        EXPECT_FALSE(session.GetWorldOpenError().empty());
        EXPECT_FALSE(session.LoadWorld(world_id));
        EXPECT_FALSE(session.GetWorldOpenError().empty());
        EXPECT_EQ(session.GetWorldSystem(), nullptr);
        const auto saved =
            Luminumbra::Persistence::InspectSavedWorld(storage.path(), assets.path(), world_id);
        EXPECT_FALSE(saved.error.empty());
        const auto catalog =
            Luminumbra::Persistence::EnumerateSavedWorlds(storage.path(), assets.path());
        ASSERT_EQ(catalog.worlds.size(), 1u);
        EXPECT_FALSE(catalog.worlds.front().error.empty());
        fs::remove_all(dependency);
        fs::rename(hidden, dependency);
    }
    jobs.shutdown();
}

TEST(GameSessionHeadlessWorldTest, MissingBiomeTableUsesLegacyFallbackForCreateReloadAndCatalog) {
    const HeadlessRoot assets;
    const HeadlessRoot storage;
    ASSERT_TRUE(fs::remove(assets.path() / "data/common/biomes.json"));
    JobSystem jobs;
    jobs.startup(1);
    std::string world_id;
    {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(assets.root_string());
        session.SetSaveRootPath(storage.path());
        ASSERT_TRUE(session.CreateWorld("LegacyBiomes", "12345", "default"))
            << session.GetWorldOpenError();
        EXPECT_FALSE(session.GetWorldSystem()->biomes_enabled());
        EXPECT_TRUE(session.GetWorldSystem()->biome_table().empty());
        world_id = session.GetMetadata().worldId;
        ASSERT_TRUE(session.SaveWorldState());
    }
    {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(assets.root_string());
        session.SetSaveRootPath(storage.path());
        ASSERT_TRUE(session.CreateTransientWorld("LegacyBiomesTransient", "12345", "default"))
            << session.GetWorldOpenError();
        EXPECT_FALSE(session.GetWorldSystem()->biomes_enabled());
        EXPECT_TRUE(session.GetWorldSystem()->biome_table().empty());
        ASSERT_TRUE(session.LoadWorld(world_id)) << session.GetWorldOpenError();
        EXPECT_FALSE(session.GetWorldSystem()->biomes_enabled());
        EXPECT_TRUE(session.GetWorldSystem()->biome_table().empty());
    }
    const auto saved =
        Luminumbra::Persistence::InspectSavedWorld(storage.path(), assets.path(), world_id);
    EXPECT_TRUE(saved.error.empty()) << saved.error;
    const auto catalog =
        Luminumbra::Persistence::EnumerateSavedWorlds(storage.path(), assets.path());
    ASSERT_EQ(catalog.worlds.size(), 1u);
    EXPECT_EQ(catalog.worlds.front().metadata.worldId, world_id);
    EXPECT_TRUE(catalog.worlds.front().error.empty()) << catalog.worlds.front().error;
    jobs.shutdown();
}

TEST(GameSessionHeadlessWorldTest, CatalogResolvesPackagedPresetsUnderRuntimeRoot) {
    const HeadlessRoot assets;
    const HeadlessRoot storage;
    fs::copy(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets-rev6",
             assets.path() / "worlds/atlas/presets-rev6",
             fs::copy_options::recursive);
    JobSystem jobs;
    jobs.startup(1);
    {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(assets.root_string());
        session.SetSaveRootPath(storage.path());
        session.SetActiveRegionsEnabled(false);
        ASSERT_TRUE(session.CreateWorld("PackagedPreset", "12345", "default"));
        const auto world_id = session.GetMetadata().worldId;
        const auto metadata = session.GetWorldSaveDir() / "world_info.json";
        nlohmann::json document;
        {
            std::ifstream input(metadata);
            input >> document;
        }
        ASSERT_TRUE(document.contains("presetFormat"));
        ASSERT_TRUE(document.contains("presetIdentity"));
        ASSERT_TRUE(fs::remove(session.GetWorldSaveDir() / "preset.json"));
        const auto refused =
            Luminumbra::Persistence::InspectSavedWorld(storage.path(), assets.path(), world_id);
        EXPECT_NE(refused.error.find("required embedded preset.json"), std::string::npos);
        const auto refused_catalog =
            Luminumbra::Persistence::EnumerateSavedWorlds(storage.path(), assets.path());
        ASSERT_EQ(refused_catalog.worlds.size(), 1u);
        EXPECT_EQ(refused_catalog.worlds.front().error, refused.error);
        // Historical saves predate the format marker, embedded preset and checksum.
        document.erase("presetFormat");
        document.erase("presetIdentity");
        {
            std::ofstream output(metadata);
            output << document.dump();
        }
        const auto saved =
            Luminumbra::Persistence::InspectSavedWorld(storage.path(), assets.path(), world_id);
        EXPECT_TRUE(saved.error.empty()) << saved.error;
        EXPECT_EQ(saved.preset_path, assets.path() / "worlds/atlas/presets-rev6/default.json");
        ASSERT_TRUE(session.LoadWorld(world_id)) << session.GetWorldOpenError();
    }
    jobs.shutdown();
}

TEST(GameSessionHeadlessWorldTest, HeadlessCreateWorldSucceedsWithSimulationAssetsOnly) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup();
    {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(root.root_string());
        // No SetRequiredClientAssets call: a headless host registers none.
        ASSERT_TRUE(session.CreateWorld("HeadlessWorld", "12345", "default"));
        EXPECT_NE(session.GetWorldSystem(), nullptr);
        EXPECT_NE(session.GetPhysicsSystem(), nullptr);
        EXPECT_FALSE(session.GetMetadata().worldId.empty());
        EXPECT_TRUE(session.ActiveRegionsEnabled());
        EXPECT_TRUE(session.AetherStateEnabled());
        EXPECT_TRUE(session.WeatherRainEnabled());
        EXPECT_TRUE(session.WeatherEventsEnabled());
        EXPECT_TRUE(session.FiniteHydrologyEnabled());
        EXPECT_EQ(session.GetWorldSystem()->debug_water_sim_resolution(), 16);
        EXPECT_NE(session.GetEnergyFieldState(), nullptr);
        const auto saved_preset =
            root.path() / "worlds/saves" / session.GetMetadata().worldId / "preset.json";
        std::ifstream input(saved_preset);
        ASSERT_TRUE(input);
        const auto resolved = nlohmann::json::parse(input);
        EXPECT_EQ(resolved.at("schema_rev"), 8);
        EXPECT_EQ(resolved.at("generation_params").at("terrain").at("surface_sampling_revision"),
                  2);
        EXPECT_TRUE(
            resolved.at("generation_params").at("terrain").at("island_mask_enabled").get<bool>());

        // World metadata landed inside the headless root.
        EXPECT_TRUE(fs::exists(root.path() / "worlds" / "saves" / session.GetMetadata().worldId /
                               "world_info.json"));
    }
    jobs.shutdown();
}

TEST(GameSessionHeadlessWorldTest, ScriptHostTracksTheSessionEnergyField) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup();
    {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(root.root_string());
        session.SetAetherStateEnabled(true);
        ASSERT_TRUE(session.CreateWorld("ScriptWorld", "12345", "default"));
        ASSERT_NE(session.GetScriptState(), nullptr);
        ASSERT_NE(session.GetEnergyFieldState(), nullptr);

        session.GetEnergyFieldState()->SetAnchorCell(0, 0);
        session.GetEnergyFieldState()->QueueDeposit(1, 0, 0, 0, 512);
        session.GetEnergyFieldState()->Tick(1);
        double value = 0.0;
        ASSERT_TRUE(session.GetScriptState()->EvalNumber(
            "return world.sample_energy_field(1, 0, 1)", value));
        EXPECT_DOUBLE_EQ(value, 2.0);
    }
    jobs.shutdown();
}

// A customized world embeds its resolved preset in its OWN save dir (no global custom files) and
// actually generates different terrain. Proves the create-world custom-params path below the UI
// callback boundary, which the UI e2e cannot reach.
TEST(GameSessionHeadlessWorldTest, CustomPresetEmbedsInSaveAndChangesTerrain) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup();
    {
        // Base world from the named preset.
        GameSession base;
        base.SetJobSystem(&jobs);
        base.SetRootPath(root.root_string());
        ASSERT_TRUE(base.CreateWorld("Base", "777", "default"));
        ASSERT_NE(base.GetWorldSystem(), nullptr);

        // Customized world: same seed, much larger amplitude (a resolved preset built off default).
        std::ifstream pf(root.path() / "worlds" / "atlas" / "presets" / "default.json");
        nlohmann::json j;
        pf >> j;
        const double orig = j["generation_params"]["terrain"]["base_amplitude"].get<double>();
        j["generation_params"]["terrain"]["base_amplitude"] = orig + 120.0;
        const std::string custom = j.dump();

        GameSession custom_world;
        custom_world.SetJobSystem(&jobs);
        custom_world.SetRootPath(root.root_string());
        ASSERT_TRUE(custom_world.CreateWorld("Custom", "777", "default", &custom));
        ASSERT_NE(custom_world.GetWorldSystem(), nullptr);
        EXPECT_FALSE(base.PresetContentHash().empty());
        EXPECT_NE(base.PresetContentHash(), custom_world.PresetContentHash());
        const auto identity = Luminumbra::world::LoadTerrainPresetFromJson(
            Luminumbra::world::ResolveEffectivePreset(
                j, Luminumbra::world::TerrainPresetUse::NewWorld),
            root.path() / "data");
        ASSERT_TRUE(identity.ok);
        EXPECT_EQ(custom_world.PresetContentHash(), identity.preset_content_hash);

        // The resolved preset is embedded in THIS world's save dir; worldType keeps the base name.
        const fs::path embedded =
            root.path() / "worlds" / "saves" / custom_world.GetMetadata().worldId / "preset.json";
        EXPECT_TRUE(fs::exists(embedded))
            << "custom preset must be embedded in the world's own save";
        EXPECT_EQ(custom_world.GetMetadata().worldType, "default")
            << "worldType records the base name";

        // The cranked amplitude must change generated terrain at some sampled point.
        const std::vector<std::pair<float, float>> pts = {{8.f, 8.f}, {41.f, 17.f}, {-33.f, 52.f}};
        bool differs = false;
        for (const auto& [x, z] : pts) {
            if (base.GetWorldSystem()->GetTerrainHeightAt(x, z) !=
                custom_world.GetWorldSystem()->GetTerrainHeightAt(x, z)) {
                differs = true;
                break;
            }
        }
        EXPECT_TRUE(differs) << "a larger amplitude override must produce different terrain";
    }
    jobs.shutdown();
}

// a save carrying a wrong-sized (non-empty, != (CHUNK+1)^3)
// SDF lattice is QUARANTINED at adoption — the chunk loads with its sdf_data
// cleared (marked for deterministic regeneration) and is never fed to the
// unit-step polygonise. A valid full lattice in the same save survives verbatim.
TEST(GameSessionHeadlessWorldTest, WrongSizedSdfLatticeIsRefusedOnLoad) {
    namespace P = Luminumbra::Persistence;
    using Luminumbra::Chunk;
    using Luminumbra::ChunkState;
    using Luminumbra::IVec3;
    using Luminumbra::WorldStreamingState;
    constexpr std::size_t kFullLattice = static_cast<std::size_t>(Luminumbra::CHUNK_SIZE_X + 1) *
                                         (Luminumbra::CHUNK_SIZE_Y + 1) *
                                         (Luminumbra::CHUNK_SIZE_Z + 1);

    const HeadlessRoot root;
    // Author a save whose region holds one CORRUPT chunk (truncated lattice)
    // and one VALID full-lattice control, both Ready with a renderable mesh.
    const fs::path save_dir = root.path() / "worlds" / "saves" / "corrupt_sdf_world";
    const IVec3 corrupt_coords(40, 0, 40);
    const IVec3 control_coords(41, 0, 40);
    {
        WorldStreamingState state;
        const auto make = [&state](const IVec3& coords) {
            auto chunk = state.get_or_create_chunk(coords);
            chunk->set_state(ChunkState::Ready);
            chunk->heightmap_data = {8.0f, 8.5f, 9.0f};
            chunk->mesh_vertices = {
                {Luminumbra::Vec3(0.0f, 1.0f, 0.0f), Luminumbra::Vec3(0.0f, 1.0f, 0.0f), 1u},
                {Luminumbra::Vec3(1.0f, 1.0f, 0.0f), Luminumbra::Vec3(0.0f, 1.0f, 0.0f), 2u},
                {Luminumbra::Vec3(0.0f, 1.0f, 1.0f), Luminumbra::Vec3(0.0f, 1.0f, 0.0f), 3u}};
            chunk->mesh_indices = {0u, 1u, 2u};
            return chunk;
        };
        make(corrupt_coords)->sdf_data = {-2.0f, -0.5f, 0.25f, 1.0f}; // 4 != full lattice
        make(control_coords)->sdf_data.assign(kFullLattice, 1.0f);    // valid (all air)
        const P::WorldSaveService service;
        std::vector<std::string> errors;
        ASSERT_TRUE(service.save_world(state, save_dir, &errors));
        ASSERT_TRUE(errors.empty());
    }

    JobSystem jobs;
    jobs.startup();
    {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(root.root_string());
        ASSERT_TRUE(session.CreateWorld("QuarantineWorld", "12345", "default"));
        session.GetWorldSystem()->SetWaterSimWindowCursor(19);
        EXPECT_FALSE(session.LoadWorldStateFrom(save_dir));
        EXPECT_EQ(session.GetWorldSystem()->GetWaterSimWindowCursor(), 19u);
        EXPECT_EQ(session.GetLastLoadedChunkCount(), 0u);
        EXPECT_FALSE(session.GetWorldOpenError().empty());
        EXPECT_TRUE(session.GetWorldSystem()->snapshot_streamed_chunks().empty());
        EXPECT_FALSE(session.SaveWorldStateTo(save_dir));
        EXPECT_FALSE(session.SaveWorld());
    }
    jobs.shutdown();
}

} // namespace

namespace {
namespace P = Luminumbra::Persistence;
namespace C = Luminumbra::Components;
namespace F = luminumbra::foliage;

std::string ReadClockTestFile(const fs::path& path) {
    // Consuming rdbuf() mutates the buffer; keep its owning stream mutable.
    std::ifstream input;
    input.open(path, std::ios::binary);
    std::ostringstream bytes;
    bytes << input.rdbuf();
    return bytes.str();
}

std::map<fs::path, std::string> ReadClockTestDirectory(const fs::path& path) {
    std::map<fs::path, std::string> bytes;
    for (const auto& entry : fs::recursive_directory_iterator(path))
        bytes.emplace(entry.path().lexically_relative(path),
                      entry.is_regular_file() ? ReadClockTestFile(entry.path()) : "");
    return bytes;
}

void WriteSavedPresetTestJson(const fs::path& path, const nlohmann::json& json) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << json.dump(2) << '\n';
    ASSERT_TRUE(output.good());
}

std::map<std::string, std::string> SavedPresetTestDiskBytes(const fs::path& root) {
    std::map<std::string, std::string> result;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        result.emplace(entry.path().lexically_relative(root).generic_string(),
                       entry.is_directory() ? "<directory>" : ReadClockTestFile(entry.path()));
    }
    return result;
}

class PendingSnapshotState;

class SavedWorldPresetTest : public testing::TestWithParam<bool> {
    HeadlessRoot root;
    JobSystem jobs;

protected:
    const HeadlessRoot& Root() const {
        return root;
    }

    void SetUp() override {
        jobs.startup(1);
    }
    void TearDown() override {
        jobs.shutdown();
    }
    void Configure(GameSession& session) {
        session.SetRootPath(root.root_string());
        session.SetJobSystem(&jobs);
        session.SetActiveRegionsEnabled(GetParam());
    }
    void CreateSnapshotSession(GameSession& session) {
        SCOPED_TRACE("create snapshot session");
        Configure(session);
        session.SetAetherStateEnabled(true);
        ASSERT_TRUE(session.CreateWorld("Snapshot preset identity", "1337", "default"));
    }
    void ExpectSnapshotReloaded(const PendingSnapshotState& pending,
                                const Luminumbra::Chunk& chunk,
                                const fs::path& save,
                                const std::string& world_id);
    void ExpectRefusedWithoutWrites(const std::string& id, const std::string& reason) {
        const auto before = SavedPresetTestDiskBytes(root.path());
        const auto inspected = P::InspectSavedWorld(root.path(), id);
        EXPECT_NE(inspected.error.find(reason), std::string::npos) << inspected.error;
        GameSession loaded;
        Configure(loaded);
        EXPECT_FALSE(loaded.LoadWorld(id));
        EXPECT_NE(loaded.GetWorldOpenError().find(reason), std::string::npos);
        EXPECT_EQ(loaded.GetWorldSystem(), nullptr);
        EXPECT_FALSE(loaded.SaveWorld());
        EXPECT_FALSE(loaded.SaveWorldState());
        EXPECT_EQ(SavedPresetTestDiskBytes(root.path()), before);
    }
};

TEST_P(SavedWorldPresetTest, NewSaveRoundTripsWithMarkerAndCanonicalIdentity) {
    GameSession created;
    Configure(created);
    ASSERT_TRUE(created.CreateWorld("Preset identity", "1337", "default"));
    const auto save = created.GetWorldSaveDir();
    const auto metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    const auto preset = nlohmann::json::parse(ReadClockTestFile(save / "preset.json"));
    EXPECT_EQ(metadata.at("presetFormat"), 1);
    EXPECT_EQ(metadata.at("presetIdentity"), P::StableChecksum(preset.dump()));
    EXPECT_TRUE(preset.at("generation_params").at("terrain").at("island_mask_enabled").get<bool>());
    // Formatting does not change the canonical identity, and no packaged copy is needed.
    ASSERT_TRUE(P::WorldSaveService::write_file_durably(save / "preset.json", preset.dump(4)));
    ASSERT_TRUE(fs::remove(Root().path() / "worlds/atlas/presets/default.json"));
    const auto before = SavedPresetTestDiskBytes(Root().path());
    const auto inspected = P::InspectSavedWorld(Root().path(), created.GetMetadata().worldId);
    ASSERT_TRUE(inspected.error.empty()) << inspected.error;
    EXPECT_EQ(inspected.preset_path, save / "preset.json");
    GameSession loaded;
    Configure(loaded);
    ASSERT_TRUE(loaded.LoadWorld(created.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_EQ(loaded.PresetContentHash(), created.PresetContentHash());
    EXPECT_EQ(SavedPresetTestDiskBytes(Root().path()), before);
    ASSERT_TRUE(loaded.SaveWorld());
    EXPECT_EQ(nlohmann::json::parse(ReadClockTestFile(save / "world_info.json")), metadata);
}

TEST_P(SavedWorldPresetTest, CustomAdditionalNamedPresetCarriesItsExactIdentity) {
    const auto named = Root().path() / "worlds/atlas/presets/other.json";
    fs::copy_file(SourcePresetPath(), named);
    auto custom = nlohmann::json::parse(ReadClockTestFile(named));
    custom["generation_params"]["terrain"]["height_offset"] = 123.0;
    const auto text = custom.dump();
    GameSession created;
    Configure(created);
    ASSERT_TRUE(created.CreateWorld("Custom identity", "1337", "other", &text));
    const auto save = created.GetWorldSaveDir();
    const auto metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    const auto embedded = nlohmann::json::parse(ReadClockTestFile(save / "preset.json"));
    EXPECT_EQ(metadata.at("presetFormat"), 1);
    EXPECT_EQ(metadata.at("presetIdentity"), P::StableChecksum(embedded.dump()));
    GameSession loaded;
    Configure(loaded);
    ASSERT_TRUE(loaded.LoadWorld(created.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_FLOAT_EQ(loaded.GetWorldSystem()->get_params().height_offset, 123.0f);

    auto tampered = embedded;
    tampered["generation_params"]["terrain"]["height_offset"] = 124.0;
    WriteSavedPresetTestJson(save / "preset.json", tampered);
    ExpectRefusedWithoutWrites(created.GetMetadata().worldId, "identity");
}

class SnapshotPresetFiles {
public:
    explicit SnapshotPresetFiles(const fs::path& destination)
        : directory(destination)
        , preset_bytes(ReadClockTestFile(directory / "preset.json"))
        , metadata_bytes(ReadClockTestFile(directory / "world_info.json"))
        , other_preset(nlohmann::json::parse(preset_bytes))
        , other_metadata(nlohmann::json::parse(metadata_bytes)) {
        other_preset["generation_params"]["terrain"]["height_offset"] = 123.0;
        other_metadata["presetIdentity"] = P::StableChecksum(other_preset.dump());
    }

    void Rewrite(const char* changed) const {
        SCOPED_TRACE(directory.generic_string());
        if (std::string(changed) != "identity") {
            ASSERT_NO_FATAL_FAILURE(
                WriteSavedPresetTestJson(directory / "preset.json", other_preset));
        }
        if (std::string(changed) != "preset") {
            ASSERT_NO_FATAL_FAILURE(
                WriteSavedPresetTestJson(directory / "world_info.json", other_metadata));
        }
    }

    void Restore() const {
        SCOPED_TRACE(directory.generic_string());
        ASSERT_TRUE(
            P::WorldSaveService::write_file_durably(directory / "preset.json", preset_bytes));
        ASSERT_TRUE(
            P::WorldSaveService::write_file_durably(directory / "world_info.json", metadata_bytes));
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
        , bytes(SavedPresetTestDiskBytes(directory)) {}

    void ExpectUnchanged() const {
        SCOPED_TRACE(directory.generic_string());
        EXPECT_EQ(SavedPresetTestDiskBytes(directory), bytes);
    }

private:
    fs::path directory;
    std::map<std::string, std::string> bytes;
};

using Luminumbra::TestSupport::ApplySnapshotTestEnergy;

void PreparePendingSnapshot(GameSession& session,
                            std::shared_ptr<Luminumbra::Chunk>& chunk,
                            bool existing_snapshot) {
    SCOPED_TRACE("prepare pending snapshot");
    chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    chunk->set_state(Luminumbra::ChunkState::Idle);
    chunk->heightmap_data = {8.0f, 9.0f, 10.0f};
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
    auto& registry = session.GetRegistry();
    const auto plant = registry.create();
    registry.emplace<C::PlantTag>(plant);
    registry.emplace<C::PlantGenomeComponent>(plant).genes.fill(0.5f);
    registry.emplace<C::PlantGrowthComponent>(plant);
    registry.emplace<C::TransformComponent>(plant).position = session.GetMetadata().spawnPoint;
    auto* energy = session.GetEnergyFieldState();
    ASSERT_NE(energy, nullptr);
    energy->SetAnchorCell(0, 0);
    energy->QueueDeposit(1, 0, 0, 0, 60000);
    ASSERT_NO_FATAL_FAILURE(ApplySnapshotTestEnergy(session, 0));
    if (existing_snapshot) {
        ASSERT_TRUE(session.SaveWorldState());
        ASSERT_FALSE(chunk->is_voxel_data_dirty());
    }

    chunk->heightmap_data.front() = 42.0f;
    chunk->mark_voxel_data_dirty();
    registry.get<C::PlantGrowthComponent>(plant).growth_points = 123;
    energy->QueueDeposit(2, 0, 0, 0, 1000);
    ASSERT_NO_FATAL_FAILURE(ApplySnapshotTestEnergy(session, 0));
}

class PendingSnapshotState {
public:
    explicit PendingSnapshotState(const GameSession& session)
        : plant_hash(session.ComputePlantSubHash())
        , energy_bytes(session.GetEnergyFieldState()->CanonicalBytes()) {}

    void ExpectUnchanged(const GameSession& session, const Luminumbra::Chunk& chunk) const {
        SCOPED_TRACE("pending snapshot state");
        EXPECT_TRUE(chunk.is_voxel_data_dirty());
        EXPECT_FLOAT_EQ(chunk.heightmap_data.front(), 42.0f);
        EXPECT_EQ(session.ComputePlantSubHash(), plant_hash);
        EXPECT_EQ(session.GetEnergyFieldState()->CanonicalBytes(), energy_bytes);
    }

    const std::string& PlantHash() const {
        return plant_hash;
    }
    const std::string& EnergyBytes() const {
        return energy_bytes;
    }

private:
    std::string plant_hash;
    std::string energy_bytes;
};

void SavedWorldPresetTest::ExpectSnapshotReloaded(const PendingSnapshotState& pending,
                                                  const Luminumbra::Chunk& chunk,
                                                  const fs::path& save,
                                                  const std::string& world_id) {
    SCOPED_TRACE(world_id);
    EXPECT_NE(ReadClockTestFile(P::WorldSaveService::plant_entities_path(save)), "");
    EXPECT_NE(ReadClockTestFile(save / "aether_state.efs"), "");
    GameSession loaded;
    Configure(loaded);
    loaded.SetAetherStateEnabled(true);
    ASSERT_TRUE(loaded.LoadWorld(world_id)) << loaded.GetWorldOpenError();
    EXPECT_EQ(loaded.ComputePlantSubHash(), pending.PlantHash());
    ASSERT_NE(loaded.GetEnergyFieldState(), nullptr);
    EXPECT_EQ(loaded.GetEnergyFieldState()->CanonicalBytes(), pending.EnergyBytes());
    Luminumbra::WorldStreamingState restored;
    std::vector<std::string> errors;
    ASSERT_TRUE(P::WorldSaveService{}.load_world(restored, save, errors));
    const auto restored_chunk = restored.find_chunk(chunk.get_id());
    ASSERT_NE(restored_chunk, nullptr);
    EXPECT_EQ(restored_chunk->heightmap_data, chunk.heightmap_data);
}

TEST_P(SavedWorldPresetTest, OpenSessionPresetChangeRefusesBeforeSnapshotWritesAndAcknowledgement) {
    for (const bool existing_snapshot : {false, true}) {
        SCOPED_TRACE(existing_snapshot);
        GameSession session;
        CreateSnapshotSession(session);
        if (HasFatalFailure())
            return;
        const auto save = session.GetWorldSaveDir();
        const SnapshotPresetFiles preset_files(save);

        std::shared_ptr<Luminumbra::Chunk> chunk;
        PreparePendingSnapshot(session, chunk, existing_snapshot);
        if (HasFatalFailure())
            return;
        const PendingSnapshotState pending(session);

        for (const auto* changed : {"preset", "identity", "both"}) {
            SCOPED_TRACE(changed);
            preset_files.Rewrite(changed);
            if (HasFatalFailure())
                return;
            const SnapshotDestinationBytes before(save);
            EXPECT_FALSE(session.SaveWorldStateTo(save));
            before.ExpectUnchanged();
            pending.ExpectUnchanged(session, *chunk);
            EXPECT_FALSE(session.SaveWorldState());
            before.ExpectUnchanged();
            pending.ExpectUnchanged(session, *chunk);
            EXPECT_FALSE(session.SaveWorld());
            before.ExpectUnchanged();
            pending.ExpectUnchanged(session, *chunk);
            preset_files.Restore();
            if (HasFatalFailure())
                return;
        }

        ASSERT_TRUE(session.SaveWorldState());
        EXPECT_FALSE(chunk->is_voxel_data_dirty());
        ExpectSnapshotReloaded(pending, *chunk, save, session.GetMetadata().worldId);
        if (HasFatalFailure())
            return;
    }
}

TEST_P(SavedWorldPresetTest, MissingEmbeddedPresetRefusesWithoutWrites) {
    GameSession created;
    Configure(created);
    ASSERT_TRUE(created.CreateWorld("Missing preset", "1337", "default"));
    fs::create_directories(Root().path() / "worlds/atlas/presets-rev6");
    fs::copy_file(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets-rev6/default.json",
                  Root().path() / "worlds/atlas/presets-rev6/default.json");
    ASSERT_TRUE(fs::remove(created.GetWorldSaveDir() / "preset.json"));
    ExpectRefusedWithoutWrites(created.GetMetadata().worldId, "required embedded preset.json");
}

TEST_P(SavedWorldPresetTest, MissingOrInvalidIdentityRefusesWithoutWrites) {
    GameSession created;
    Configure(created);
    ASSERT_TRUE(created.CreateWorld("Missing identity", "1337", "default"));
    const auto path = created.GetWorldSaveDir() / "world_info.json";
    auto metadata = nlohmann::json::parse(ReadClockTestFile(path));
    metadata.erase("presetIdentity");
    WriteSavedPresetTestJson(path, metadata);
    ExpectRefusedWithoutWrites(created.GetMetadata().worldId, "identity");
    for (const auto& identity : {nlohmann::json(nullptr),
                                 nlohmann::json(123),
                                 nlohmann::json(""),
                                 nlohmann::json("wrong")}) {
        SCOPED_TRACE(identity.dump());
        metadata["presetIdentity"] = identity;
        WriteSavedPresetTestJson(path, metadata);
        ExpectRefusedWithoutWrites(created.GetMetadata().worldId, "identity");
    }
}

TEST_P(SavedWorldPresetTest, TamperedEmbeddedPresetRefusesOnIdentityWithoutWrites) {
    GameSession created;
    Configure(created);
    ASSERT_TRUE(created.CreateWorld("Tampered preset", "1337", "default"));
    const auto path = created.GetWorldSaveDir() / "preset.json";
    auto preset = nlohmann::json::parse(ReadClockTestFile(path));
    preset["generation_params"]["terrain"]["island_mask_enabled"] = false;
    WriteSavedPresetTestJson(path, preset);
    ExpectRefusedWithoutWrites(created.GetMetadata().worldId, "identity");
}

TEST_P(SavedWorldPresetTest, InvalidFormatNeverFallsBack) {
    GameSession created;
    Configure(created);
    ASSERT_TRUE(created.CreateWorld("Invalid format", "1337", "default"));
    const auto save = created.GetWorldSaveDir();
    auto metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    ASSERT_TRUE(fs::remove(save / "preset.json"));
    for (const auto& format : {nlohmann::json(nullptr),
                               nlohmann::json(false),
                               nlohmann::json(0),
                               nlohmann::json(2),
                               nlohmann::json("1")}) {
        SCOPED_TRACE(format.dump());
        metadata["presetFormat"] = format;
        WriteSavedPresetTestJson(save / "world_info.json", metadata);
        ExpectRefusedWithoutWrites(created.GetMetadata().worldId, "preset format");
    }
}

TEST_P(SavedWorldPresetTest, UnmarkedLegacySaveUsesFrozenPresetAndEmbedsItOnResave) {
    const auto historical = Root().path() / "worlds/atlas/presets-rev6/default.json";
    fs::create_directories(historical.parent_path());
    fs::copy_file(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets-rev6/default.json",
                  historical);
    const auto save = Root().path() / "worlds/saves/legacy";
    fs::create_directories(save);
    WriteSavedPresetTestJson(save / "world_info.json",
                             {{"container_version", 2}, {"worldType", "default"}});
    const auto before = SavedPresetTestDiskBytes(Root().path());
    const auto inspected = P::InspectSavedWorld(Root().path(), "legacy");
    ASSERT_TRUE(inspected.error.empty()) << inspected.error;
    EXPECT_EQ(inspected.preset_path, historical);
    GameSession loaded;
    Configure(loaded);
    ASSERT_TRUE(loaded.LoadWorld("legacy")) << loaded.GetWorldOpenError();
    EXPECT_FALSE(loaded.GetWorldSystem()->get_params().island_mask_enabled);
    EXPECT_EQ(SavedPresetTestDiskBytes(Root().path()), before);
    ASSERT_TRUE(loaded.SaveWorld());
    const auto metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    EXPECT_EQ(metadata.at("presetFormat"), 1);
    EXPECT_EQ(metadata.at("presetIdentity"),
              P::StableChecksum(nlohmann::json::parse(ReadClockTestFile(historical)).dump()));
    EXPECT_EQ(nlohmann::json::parse(ReadClockTestFile(save / "preset.json")),
              nlohmann::json::parse(ReadClockTestFile(historical)));
    ASSERT_TRUE(fs::remove(historical));
    EXPECT_TRUE(P::InspectSavedWorld(Root().path(), "legacy").error.empty());
}

nlohmann::json SessionWorldSubHashes(GameSession& session);

// The base consumes no structure pool. Only the custom candidate opts in, so
// named-preset admission cannot mask the dependency refusal under test.
nlohmann::json PrepareCreationRefusalBase(const fs::path& root) {
    auto base =
        nlohmann::json::parse(ReadClockTestFile(root / "worlds/atlas/presets/default.json"));
    base["generation_params"]["features"]["structures_enabled"] = false;
    base["generation_params"]["biomes"]["table"] = "";
    WriteSavedPresetTestJson(root / "worlds/atlas/presets/default.json", base);
    fs::remove_all(root / "data/common/structures");
    return base;
}

std::array<std::string, 3> CreationRefusalInputs(const nlohmann::json& base) {
    auto semantic = base;
    semantic["generation_params"]["terrain"]["height_offset"] = "invalid height";
    auto dependency = base;
    dependency["generation_params"]["features"]["structures_enabled"] = true;
    return {"{", semantic.dump(), dependency.dump()};
}

class RetainedCreationState {
public:
    explicit RetainedCreationState(GameSession& session)
        : world(session.GetWorldSystem())
        , water(session.GetWaterSystem())
        , physics(session.GetPhysicsSystem())
        , species(session.GetSpeciesTable())
        , dependencies(world->get_params().worldgen_dependencies)
        , metadata(session.GetMetadata())
        , identity(session.PresetContentHash())
        , generated_identity(
              Luminumbra::World::ComputeTerrainParamsHash(world->get_params(), world->get_seed()))
        , hashes(SessionWorldSubHashes(session))
        , tick(session.GetSimulationTickCount())
        , ledger(session.GetActiveRegionLedger().encode()) {}

    void ExpectUnchanged(GameSession& session) const {
        EXPECT_EQ(session.GetWorldSystem(), world);
        EXPECT_EQ(session.GetWaterSystem(), water);
        EXPECT_EQ(session.GetPhysicsSystem(), physics);
        EXPECT_EQ(session.GetSpeciesTable(), species);
        EXPECT_EQ(session.GetWorldSystem()->get_params().worldgen_dependencies, dependencies);
        EXPECT_EQ(session.GetMetadata().name, metadata.name);
        EXPECT_EQ(session.GetMetadata().seed, metadata.seed);
        EXPECT_EQ(session.GetMetadata().worldType, metadata.worldType);
        EXPECT_EQ(session.GetMetadata().worldId, metadata.worldId);
        EXPECT_EQ(session.GetMetadata().creationTime, metadata.creationTime);
        EXPECT_EQ(session.GetMetadata().spawnPoint, metadata.spawnPoint);
        EXPECT_EQ(session.PresetContentHash(), identity);
        EXPECT_EQ(Luminumbra::World::ComputeTerrainParamsHash(
                      session.GetWorldSystem()->get_params(), session.GetWorldSystem()->get_seed()),
                  generated_identity);
        EXPECT_EQ(SessionWorldSubHashes(session), hashes);
        EXPECT_EQ(session.GetSimulationTickCount(), tick);
        EXPECT_EQ(session.GetActiveRegionLedger().encode(), ledger);
    }

private:
    const Luminumbra::Systems::SHIELD_WorldSystem* world;
    const Luminumbra::Systems::WaterSystem* water;
    const Luminumbra::Systems::PhysicsSystem* physics;
    const luminumbra::ai::CreatureSpeciesRegistry* species;
    std::shared_ptr<const Luminumbra::World::WorldgenDependencySnapshot> dependencies;
    Luminumbra::world::WorldMetadata metadata;
    std::string identity;
    std::uint64_t generated_identity;
    nlohmann::json hashes;
    std::uint64_t tick;
    std::string ledger;
};

TEST_P(SavedWorldPresetTest, NormalCreationRefusalsRetainDirtySessionAndExitSave) {
    const auto base = PrepareCreationRefusalBase(Root().path());
    ASSERT_TRUE(GameSession::ValidateWorldConfig(Root().root_string(), "default").ok);
    const auto resolved = Luminumbra::world::ResolveEffectivePreset(
        base, Luminumbra::world::TerrainPresetUse::NewWorld);
    const auto loaded_base =
        Luminumbra::world::LoadTerrainPresetFromJson(resolved, Root().path() / "data");
    ASSERT_TRUE(loaded_base.ok);
    ASSERT_FALSE(loaded_base.params.structures_enabled);
    const auto invalid = CreationRefusalInputs(base);
    const auto dependency = Luminumbra::world::LoadTerrainPresetFromJson(
        nlohmann::json::parse(invalid.back()), Root().path() / "data");
    ASSERT_FALSE(dependency.ok);
    ASSERT_FALSE(dependency.errors.empty());
    ASSERT_NE(dependency.errors.front().find("structure pools"), std::string::npos);

    // Both the first snapshot and the incremental snapshot must remain saveable.
    for (const bool existing_snapshot : {false, true}) {
        for (const auto& custom : invalid) {
            SCOPED_TRACE(existing_snapshot);
            SCOPED_TRACE(custom);
            GameSession session;
            CreateSnapshotSession(session);
            ASSERT_FALSE(HasFatalFailure());
            for (int tick = 0; tick < 3; ++tick)
                ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
            ASSERT_GT(session.GetSimulationTickCount(), 0u);
            if (GetParam()) {
                ASSERT_GT(session.GetActiveRegionLedger().tick(), 0u);
            }
            std::shared_ptr<Luminumbra::Chunk> chunk;
            PreparePendingSnapshot(session, chunk, existing_snapshot);
            ASSERT_FALSE(HasFatalFailure());
            const PendingSnapshotState pending(session);
            const RetainedCreationState retained(session);
            const auto disk = SavedPresetTestDiskBytes(Root().path());
            const auto save = session.GetWorldSaveDir();
            const auto id = session.GetMetadata().worldId;
            EXPECT_FALSE(session.CreateWorld("Refused replacement", "999", "default", &custom));
            EXPECT_FALSE(session.GetWorldOpenError().empty());
            retained.ExpectUnchanged(session);
            pending.ExpectUnchanged(session, *chunk);
            // Includes every directory, even an orphan with no catalog metadata.
            EXPECT_EQ(SavedPresetTestDiskBytes(Root().path()), disk);

            bool spawn_updated = false;
            bool saved_reported = false;
            Luminumbra::Client::SaveSessionOnExit(
                &session,
                true,
                [&] {
                    session.SetSpawnPoint(Luminumbra::Vec3(19, 23, -17));
                    spawn_updated = true;
                },
                [&] {
                    EXPECT_TRUE(spawn_updated);
                    ExpectSnapshotReloaded(pending, *chunk, save, id);
                    EXPECT_FALSE(chunk->is_voxel_data_dirty());
                    const auto info =
                        nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
                    EXPECT_EQ(info.at("spawnPoint").at("x"), 19);
                    saved_reported = true;
                });
            ASSERT_TRUE(spawn_updated);
            ASSERT_TRUE(saved_reported);
            EXPECT_FALSE(session.GetWorldOpenError().empty());
            // Explicit manual/autosave entry points also work without clearing diagnostics.
            ASSERT_TRUE(session.SaveWorldState());
            ASSERT_TRUE(session.SaveWorld());
            ExpectSnapshotReloaded(pending, *chunk, save, id);
            GameSession reopened;
            Configure(reopened);
            ASSERT_TRUE(reopened.LoadWorld(id)) << reopened.GetWorldOpenError();
            EXPECT_EQ(reopened.GetSimulationTickCount(), GetParam() ? 3u : 0u);
            if (GetParam()) {
                EXPECT_EQ(reopened.GetActiveRegionLedger().encode(),
                          session.GetActiveRegionLedger().encode());
            }
        }
    }
}

TEST_P(SavedWorldPresetTest, RefusedOpenAndMissingJobsPreserveSaveEligibility) {
    GameSession session;
    CreateSnapshotSession(session);
    ASSERT_FALSE(HasFatalFailure());
    std::shared_ptr<Luminumbra::Chunk> chunk;
    PreparePendingSnapshot(session, chunk, false);
    ASSERT_FALSE(HasFatalFailure());
    const PendingSnapshotState pending(session);
    const RetainedCreationState retained(session);
    const auto disk = SavedPresetTestDiskBytes(Root().path());
    const auto id = session.GetMetadata().worldId;
    session.SetJobSystem(nullptr);
    EXPECT_FALSE(session.CreateWorld("No jobs", "999", "default"));
    EXPECT_NE(session.GetWorldOpenError().find("JobSystem"), std::string::npos);
    EXPECT_FALSE(session.LoadWorld(id));
    EXPECT_NE(session.GetWorldOpenError().find("JobSystem"), std::string::npos);
    Configure(session);
    EXPECT_FALSE(session.CreateWorld("Missing named preset", "999", "missing-preset"));
    EXPECT_FALSE(session.CreateWorld("Recording mismatch",
                                     "999",
                                     "default",
                                     nullptr,
                                     Luminumbra::world::TerrainPresetUse::Historical,
                                     "different-content"));
    session.SetRequiredClientAssets({"missing-required-asset"});
    EXPECT_FALSE(session.CreateWorld("Missing asset", "999", "default"));
    EXPECT_FALSE(session.LoadWorld(id));
    session.SetRequiredClientAssets({});
    EXPECT_FALSE(session.LoadWorld("missing-world"));
    EXPECT_FALSE(session.GetWorldOpenError().empty());
    retained.ExpectUnchanged(session);
    pending.ExpectUnchanged(session, *chunk);
    EXPECT_EQ(SavedPresetTestDiskBytes(Root().path()), disk);
    ASSERT_TRUE(session.SaveWorldState());
    ASSERT_TRUE(session.SaveWorld());
    ExpectSnapshotReloaded(pending, *chunk, session.GetWorldSaveDir(), id);
}

TEST_P(SavedWorldPresetTest, CandidateLatticeRefusalPreservesLiveWorldAndSaveEligibility) {
    for (const bool indexed_candidate : {false, true}) {
        GameSession live, candidate;
        ASSERT_NO_FATAL_FAILURE(CreateSnapshotSession(live));
        Configure(candidate);
        candidate.SetActiveRegionsEnabled(indexed_candidate);
        ASSERT_TRUE(candidate.CreateWorld("Candidate lattice", "7331", "default"));
        const auto candidate_save = candidate.GetWorldSaveDir();
        Luminumbra::WorldStreamingState snapshot;
        auto malformed = snapshot.get_or_create_chunk(Luminumbra::IVec3(40, 0, 40));
        malformed->set_state(Luminumbra::ChunkState::Idle);
        malformed->sdf_data.assign(17u * 17u * 17u, -2.0f);
        ASSERT_TRUE(P::WorldSaveService{}.save_world(snapshot, candidate_save));
        // Publish a valid baseline, then inject the defect through a separate legacy encoder.
        // Indexed writers must refuse malformed lattice publication themselves.
        malformed->sdf_data = {-2.0f, 0.25f};
        const auto corrupt = Root().path() / "malformed-candidate";
        ASSERT_TRUE(P::WorldSaveService{}.save_world(snapshot, corrupt));
        fs::copy_file(P::WorldSaveService::region_file_path(corrupt, 1, 1),
                      P::WorldSaveService::region_file_path(candidate_save, 1, 1),
                      fs::copy_options::overwrite_existing);
        const auto inspected = P::InspectSavedWorld(Root().path(), candidate.GetMetadata().worldId);
        if (indexed_candidate) {
            ASSERT_EQ(inspected.error, "corrupt world chunk SDF lattice");
        } else {
            // Legacy containers reach the session's final lattice check after catalog admission.
            ASSERT_TRUE(inspected.error.empty());
        }
        std::shared_ptr<Luminumbra::Chunk> chunk;
        ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(live, chunk, true));
        const PendingSnapshotState pending(live);
        const RetainedCreationState retained(live);
        const auto disk = SavedPresetTestDiskBytes(Root().path());
        for (const bool explicit_root : {false, true}) {
            SCOPED_TRACE(explicit_root);
            EXPECT_FALSE(explicit_root
                             ? live.LoadWorldFrom(candidate.GetMetadata().worldId, Root().path())
                             : live.LoadWorld(candidate.GetMetadata().worldId));
            EXPECT_EQ(live.GetWorldOpenError(), "corrupt world chunk SDF lattice");
            retained.ExpectUnchanged(live);
            pending.ExpectUnchanged(live, *chunk);
            const auto residents = live.GetWorldSystem()->snapshot_streamed_chunks();
            ASSERT_EQ(residents.size(), 1u);
            EXPECT_EQ(residents.front(), chunk);
            EXPECT_EQ(SavedPresetTestDiskBytes(Root().path()), disk);
        }
        ASSERT_TRUE(live.SaveWorld());
        EXPECT_FALSE(chunk->is_voxel_data_dirty());
        ASSERT_NO_FATAL_FAILURE(ExpectSnapshotReloaded(
            pending, *chunk, live.GetWorldSaveDir(), live.GetMetadata().worldId));
    }
}

TEST_P(SavedWorldPresetTest, CandidateSchedulerRefusalsPreservePendingLiveState) {
    GameSession live, candidate;
    live.SetRegionSchedulerConfig({2, 2, 1});
    ASSERT_NO_FATAL_FAILURE(CreateSnapshotSession(live));
    Configure(candidate);
    candidate.SetActiveRegionsEnabled(true);
    candidate.SetRegionSchedulerConfig({1, 2, 1});
    ASSERT_TRUE(candidate.CreateWorld("Candidate scheduler", "7331", "default"));
    const auto save = candidate.GetWorldSaveDir();
    const auto path = P::WorldSaveService::active_regions_path(save);
    const auto valid = ReadClockTestFile(path);
    std::shared_ptr<Luminumbra::Chunk> chunk;
    ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(live, chunk, true));
    const PendingSnapshotState pending(live);
    const RetainedCreationState retained(live);
    for (const bool malformed : {false, true}) {
        SCOPED_TRACE(malformed);
        ASSERT_TRUE(
            P::WorldSaveService::write_file_durably(path, malformed ? std::string("ARL") : valid));
        const auto disk = SavedPresetTestDiskBytes(Root().path());
        for (const bool explicit_root : {false, true}) {
            SCOPED_TRACE(explicit_root);
            EXPECT_FALSE(explicit_root
                             ? live.LoadWorldFrom(candidate.GetMetadata().worldId, Root().path())
                             : live.LoadWorld(candidate.GetMetadata().worldId));
            if (GetParam() && !malformed) {
                EXPECT_NE(live.GetWorldOpenError().find("scheduler differs"), std::string::npos);
            } else {
                EXPECT_FALSE(live.GetWorldOpenError().empty());
            }
            retained.ExpectUnchanged(live);
            pending.ExpectUnchanged(live, *chunk);
            const auto residents = live.GetWorldSystem()->snapshot_streamed_chunks();
            ASSERT_EQ(residents.size(), 1u);
            EXPECT_EQ(residents.front(), chunk);
            EXPECT_EQ(SavedPresetTestDiskBytes(Root().path()), disk);
        }
    }
    ASSERT_TRUE(live.SaveWorld());
    EXPECT_FALSE(chunk->is_voxel_data_dirty());
    ASSERT_NO_FATAL_FAILURE(ExpectSnapshotReloaded(
        pending, *chunk, live.GetWorldSaveDir(), live.GetMetadata().worldId));
}

TEST_P(SavedWorldPresetTest, InvalidLivePlantRefusesBeforeAnySnapshotPayloadWrite) {
    GameSession session;
    ASSERT_NO_FATAL_FAILURE(CreateSnapshotSession(session));
    std::shared_ptr<Luminumbra::Chunk> chunk;
    ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(session, chunk, true));
    auto plants = session.GetRegistry().view<C::PlantTag, C::PlantGenomeComponent>();
    ASSERT_NE(plants.begin(), plants.end());
    plants.get<C::PlantGenomeComponent>(*plants.begin()).genes[0] =
        std::numeric_limits<float>::infinity();
    const PendingSnapshotState pending(session);
    const RetainedCreationState retained(session);
    const auto disk = SavedPresetTestDiskBytes(Root().path());
    const auto destination = Root().path() / "invalid-live-plant";
    EXPECT_FALSE(session.SaveWorldStateTo(destination));
    EXPECT_FALSE(fs::exists(destination));
    EXPECT_FALSE(session.SaveWorldState());
    retained.ExpectUnchanged(session);
    pending.ExpectUnchanged(session, *chunk);
    EXPECT_EQ(SavedPresetTestDiskBytes(Root().path()), disk);
}

TEST_P(SavedWorldPresetTest, InvalidPlantProjectionRefusesAllLoadPathsWithoutAdoptionOrWrites) {
    GameSession session;
    ASSERT_NO_FATAL_FAILURE(CreateSnapshotSession(session));
    std::shared_ptr<Luminumbra::Chunk> chunk;
    ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(session, chunk, true));
    const PendingSnapshotState pending(session);
    const RetainedCreationState retained(session);
    const auto save = session.GetWorldSaveDir();
    const auto path = P::WorldSaveService::plant_entities_path(save);
    auto snapshot = luminumbra::foliage::BuildPlantEntitySnapshot(session.GetRegistry());
    ASSERT_FALSE(snapshot.entities.empty());
    for (auto& component : snapshot.entities.front().components) {
        if (component.type == "PlantGrowth")
            component.data["stage"] = 1e38;
    }
    ASSERT_TRUE(P::WorldSaveService::write_file_durably(
        path, Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(snapshot)));
    const auto disk = SavedPresetTestDiskBytes(Root().path());
    const auto world_id = session.GetMetadata().worldId;
    for (int entry = 0; entry < 3; ++entry) {
        SCOPED_TRACE(entry);
        const bool loaded = entry == 0   ? session.LoadWorld(world_id)
                            : entry == 1 ? session.LoadWorldFrom(world_id, Root().path())
                                         : session.LoadWorldStateFrom(save);
        EXPECT_FALSE(loaded);
        EXPECT_NE(session.GetWorldOpenError().find("invalid plant component"), std::string::npos);
        retained.ExpectUnchanged(session);
        pending.ExpectUnchanged(session, *chunk);
        EXPECT_EQ(SavedPresetTestDiskBytes(Root().path()), disk);
    }
    // A refused load did not disable saving a coherent live session to another destination.
    EXPECT_TRUE(session.SaveWorldStateTo(Root().path() / "after-refused-plant"));
}

TEST_P(SavedWorldPresetTest, GenericPartialPlantRecordsRefuseAllLoadPathsBeforeAdoption) {
    GameSession session;
    ASSERT_NO_FATAL_FAILURE(CreateSnapshotSession(session));
    std::shared_ptr<Luminumbra::Chunk> chunk;
    ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(session, chunk, true));
    const PendingSnapshotState pending(session);
    const RetainedCreationState retained(session);
    const auto save = session.GetWorldSaveDir();
    const auto original = luminumbra::foliage::BuildPlantEntitySnapshot(session.GetRegistry());
    ASSERT_FALSE(original.entities.empty());
    const auto world_id = session.GetMetadata().worldId;
    for (unsigned mask = 0; mask < 7; ++mask) {
        SCOPED_TRACE(mask);
        auto snapshot = original;
        std::erase_if(snapshot.entities.front().components, [mask](const auto& component) {
            return (component.type == "Transform" && (mask & 1u) == 0) ||
                   (component.type == "PlantGenome" && (mask & 2u) == 0) ||
                   (component.type == "PlantGrowth" && (mask & 4u) == 0);
        });
        ASSERT_TRUE(P::WorldSaveService::save_plant_entities(snapshot, save));
        const auto disk = SavedPresetTestDiskBytes(Root().path());
        for (int entry = 0; entry < 3; ++entry) {
            SCOPED_TRACE(entry);
            const bool loaded = entry == 0   ? session.LoadWorld(world_id)
                                : entry == 1 ? session.LoadWorldFrom(world_id, Root().path())
                                             : session.LoadWorldStateFrom(save);
            EXPECT_FALSE(loaded);
            EXPECT_NE(session.GetWorldOpenError().find("invalid plant snapshot"),
                      std::string::npos);
            retained.ExpectUnchanged(session);
            pending.ExpectUnchanged(session, *chunk);
            EXPECT_EQ(SavedPresetTestDiskBytes(Root().path()), disk);
        }
    }
}

#if !defined(_WIN32)
TEST_P(SavedWorldPresetTest, NonRegularPlantFilesRefuseAllLoadPathsBeforeAdoption) {
    GameSession session;
    ASSERT_NO_FATAL_FAILURE(CreateSnapshotSession(session));
    std::shared_ptr<Luminumbra::Chunk> chunk;
    ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(session, chunk, true));
    const PendingSnapshotState pending(session);
    const RetainedCreationState retained(session);
    const auto save = session.GetWorldSaveDir();
    const auto path = P::WorldSaveService::plant_entities_path(save);
    const auto snapshot = luminumbra::foliage::BuildPlantEntitySnapshot(session.GetRegistry());
    ASSERT_FALSE(snapshot.entities.empty());
    const auto bytes = Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(snapshot);
    const auto target = save / "valid-plant-roster.json";
    ASSERT_TRUE(P::WorldSaveService::write_file_durably(target, bytes));
    const auto world_id = session.GetMetadata().worldId;
    for (const int kind : {0, 1, 2, 3}) {
        SCOPED_TRACE(kind);
        ASSERT_TRUE(fs::remove(path));
        if (kind == 0) {
            ASSERT_EQ(::mkfifo(path.c_str(), 0600), 0);
        } else {
            fs::create_symlink(kind == 1   ? target
                               : kind == 2 ? fs::path("/dev/zero")
                                           : save / "absent-roster",
                               path);
        }
        const int keeper = kind == 0 ? ::open(path.c_str(), O_RDONLY | O_NONBLOCK) : -1;
        struct CloseReader {
            int descriptor;
            ~CloseReader() {
                if (descriptor >= 0)
                    ::close(descriptor);
            }
        } close_reader{keeper};
        if (kind == 0) {
            ASSERT_GE(keeper, 0);
        }
        for (int entry = 0; entry < 3; ++entry) {
            SCOPED_TRACE(entry);
            if (kind == 0) {
                const int writer = ::open(path.c_str(), O_WRONLY | O_NONBLOCK);
                ASSERT_GE(writer, 0);
                const auto written = ::write(writer, bytes.data(), bytes.size());
                const int closed = ::close(writer);
                ASSERT_EQ(written, static_cast<ssize_t>(bytes.size()));
                ASSERT_EQ(closed, 0);
            }
            const bool loaded = entry == 0   ? session.LoadWorld(world_id)
                                : entry == 1 ? session.LoadWorldFrom(world_id, Root().path())
                                             : session.LoadWorldStateFrom(save);
            if (kind == 0) {
                // Each entry point sees one valid JSON payload, even when the
                // preceding refusal left all FIFO bytes unread.
                std::string unread(bytes.size(), '\0');
                EXPECT_GE(::read(keeper, unread.data(), unread.size()), 0);
            }
            ASSERT_FALSE(loaded);
            EXPECT_FALSE(session.GetWorldOpenError().empty());
            retained.ExpectUnchanged(session);
            pending.ExpectUnchanged(session, *chunk);
        }
    }
}
#endif

TEST_P(SavedWorldPresetTest, FatalSnapshotAndFinalMetadataRefusalsCannotBeClearedByCreation) {
    const auto invalid = std::string("{");
    for (const bool metadata_failure : {false, true}) {
        SCOPED_TRACE(metadata_failure);
        GameSession session;
        Configure(session);
        if (metadata_failure) {
            // The generic replacement seam also intercepts preset publication.
            // The existing ledger hook reaches the final metadata save after adoption.
            session.SetActiveRegionsEnabled(true);
            P::WorldSaveService::set_before_active_regions_replace_for_testing(
                [] { return false; });
            const bool created = session.CreateWorld("Incomplete metadata", "1337", "default");
            P::WorldSaveService::set_before_active_regions_replace_for_testing(nullptr);
            ASSERT_FALSE(created);
            ASSERT_NE(session.GetWorldOpenError().find("metadata"), std::string::npos);
        } else {
            ASSERT_TRUE(session.CreateWorld("Corrupt snapshot", "1337", "default"));
            const auto corrupt = Root().path() / "corrupt-state";
            ASSERT_TRUE(P::WorldSaveService::write_file_durably(corrupt / "world_info.json", "{"));
            ASSERT_FALSE(session.LoadWorldStateFrom(corrupt));
            ASSERT_FALSE(session.GetWorldOpenError().empty());
        }
        ASSERT_NE(session.GetWorldSystem(), nullptr);
        const auto disk = SavedPresetTestDiskBytes(Root().path());
        EXPECT_FALSE(session.SaveWorldState());
        EXPECT_FALSE(session.SaveWorld());
        EXPECT_FALSE(session.CreateWorld("Refused", "999", "default", &invalid));
        EXPECT_FALSE(session.SaveWorldState());
        EXPECT_FALSE(session.SaveWorld());
        session.SetJobSystem(nullptr);
        EXPECT_FALSE(session.CreateWorld("Missing jobs", "999", "default"));
        EXPECT_NE(session.GetWorldOpenError().find("JobSystem"), std::string::npos);
        EXPECT_FALSE(session.SaveWorldState());
        EXPECT_FALSE(session.SaveWorld());
        EXPECT_FALSE(session.LoadWorldState());
        unsigned spawn_updates = 0;
        Luminumbra::Client::SaveSessionOnExit(
            &session,
            true,
            [&] { ++spawn_updates; },
            [] { FAIL() << "Fatal session reported save success"; });
        EXPECT_EQ(spawn_updates, 1u);
        EXPECT_EQ(SavedPresetTestDiskBytes(Root().path()), disk);
        Configure(session);
        EXPECT_FALSE(session.LoadWorld("missing-world"));
        EXPECT_FALSE(session.SaveWorldState());
        EXPECT_FALSE(session.SaveWorld());
        ASSERT_TRUE(session.CreateWorld("Coherent replacement", "1337", "default"));
        EXPECT_TRUE(session.GetWorldOpenError().empty());
        EXPECT_TRUE(session.SaveWorldState());
        EXPECT_TRUE(session.SaveWorld());
    }
}

TEST_P(SavedWorldPresetTest, FreshAndTransientRefusalsAndExactCustomReplacement) {
    const auto base = PrepareCreationRefusalBase(Root().path());
    const auto invalid = CreationRefusalInputs(base);
    GameSession session;
    Configure(session);
    for (const auto& custom : invalid) {
        const auto disk = SavedPresetTestDiskBytes(Root().path());
        EXPECT_FALSE(session.CreateWorld("Fresh refusal", "999", "default", &custom));
        EXPECT_FALSE(session.GetWorldOpenError().empty());
        EXPECT_EQ(session.GetWorldSystem(), nullptr);
        EXPECT_EQ(session.GetSpeciesTable(), nullptr);
        EXPECT_TRUE(session.PresetContentHash().empty());
        EXPECT_TRUE(session.GetMetadata().worldId.empty());
        EXPECT_FALSE(session.SaveWorldState());
        EXPECT_FALSE(session.SaveWorld());
        EXPECT_EQ(SavedPresetTestDiskBytes(Root().path()), disk);
    }
    auto semantic = nlohmann::json::parse(invalid[1]);
    WriteSavedPresetTestJson(Root().path() / "worlds/atlas/presets/default.json", semantic);
    const auto fresh_disk = SavedPresetTestDiskBytes(Root().path());
    EXPECT_FALSE(session.CreateTransientWorld("Fresh transient refusal", "1337", "default"));
    EXPECT_EQ(session.GetWorldSystem(), nullptr);
    EXPECT_FALSE(session.LoadWorldState());
    EXPECT_EQ(SavedPresetTestDiskBytes(Root().path()), fresh_disk);
    WriteSavedPresetTestJson(Root().path() / "worlds/atlas/presets/default.json", base);
    ASSERT_TRUE(session.CreateTransientWorld("Menu", "1337", "default"));
    const RetainedCreationState retained(session);
    WriteSavedPresetTestJson(Root().path() / "worlds/atlas/presets/default.json", semantic);
    const auto disk = SavedPresetTestDiskBytes(Root().path());
    EXPECT_FALSE(session.CreateTransientWorld("Refused menu", "999", "default"));
    retained.ExpectUnchanged(session);
    EXPECT_TRUE(session.LoadWorldState());
    EXPECT_FALSE(session.SaveWorld());
    EXPECT_FALSE(session.SaveWorldState());
    EXPECT_EQ(SavedPresetTestDiskBytes(Root().path()), disk);
    WriteSavedPresetTestJson(Root().path() / "worlds/atlas/presets/default.json", base);
    auto custom = base;
    custom["generation_params"]["terrain"]["height_offset"] = 123.0;
    custom["generation_params"]["terrain"].erase("surface_sampling_revision");
    const auto text = custom.dump();
    const auto resolved = Luminumbra::world::ResolveEffectivePreset(
        custom, Luminumbra::world::TerrainPresetUse::NewWorld);
    const auto expected =
        Luminumbra::world::LoadTerrainPresetFromJson(resolved, Root().path() / "data");
    ASSERT_TRUE(expected.ok);
    ASSERT_TRUE(session.CreateWorld("Valid replacement", "777", "default", &text));
    EXPECT_TRUE(session.GetWorldOpenError().empty());
    const auto save = session.GetWorldSaveDir();
    EXPECT_EQ(ReadClockTestFile(save / "preset.json"), resolved.dump() + "\n");
    EXPECT_EQ(session.PresetContentHash(), expected.preset_content_hash);
    EXPECT_EQ(session.GetWorldSystem()->get_params().surface_sampling_revision, 2);
    EXPECT_FLOAT_EQ(session.GetWorldSystem()->get_params().height_offset, 123.0f);
    const auto info = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    EXPECT_EQ(info.at("presetIdentity"), P::StableChecksum(resolved.dump()));
    EXPECT_EQ(info.at("worldType"), "default");
    GameSession reopened;
    Configure(reopened);
    ASSERT_TRUE(reopened.LoadWorld(session.GetMetadata().worldId)) << reopened.GetWorldOpenError();
    EXPECT_EQ(reopened.PresetContentHash(), expected.preset_content_hash);
    EXPECT_EQ(reopened.GetWorldSystem()->get_params().surface_sampling_revision, 2);
    EXPECT_FLOAT_EQ(reopened.GetWorldSystem()->get_params().height_offset, 123.0f);
}

TEST_P(SavedWorldPresetTest, SnapshotLoadRestoresWaterCursorAndDefaultsMissingMetadataToZero) {
    GameSession source, reader;
    Configure(source);
    Configure(reader);
    ASSERT_TRUE(source.CreateWorld("Water cursor source", "1337", "default"));
    ASSERT_TRUE(reader.CreateTransientWorld("Water cursor reader", "1337", "default"));
    // A value beyond the 64-chunk update cap must survive exactly; clamping it
    // would choose a different next water window when more chunks are awake.
    constexpr std::size_t saved_cursor = 65;
    source.GetWorldSystem()->SetWaterSimWindowCursor(saved_cursor);
    ASSERT_TRUE(source.SaveWorld());
    const auto source_save = source.GetWorldSaveDir();
    ASSERT_TRUE(source.LoadWorld(source.GetMetadata().worldId)) << source.GetWorldOpenError();
    EXPECT_EQ(source.GetWorldSystem()->GetWaterSimWindowCursor(), saved_cursor);

    const auto snapshot = Root().path() / "water-cursor-snapshot";
    for (const bool with_chunks : {false, true}) {
        SCOPED_TRACE(with_chunks);
        fs::remove_all(snapshot);
        ASSERT_TRUE(fs::create_directories(snapshot));
        fs::copy_file(source_save / "preset.json", snapshot / "preset.json");
        Luminumbra::WorldStreamingState state;
        if (with_chunks) {
            auto chunk = state.get_or_create_chunk(Luminumbra::IVec3(0, 0, 0));
            chunk->set_state(Luminumbra::ChunkState::Idle);
        }
        ASSERT_TRUE(Luminumbra::Test::PublishIndexedSave(snapshot, [&] {
            return P::WorldSaveService::write_member_bytes(
                       snapshot,
                       "world_info.json",
                       ReadClockTestFile(source_save / "world_info.json")) &&
                   P::WorldSaveService::remove_member(snapshot,
                                                      "chunks/region/active-regions.arl") &&
                   (!with_chunks || P::WorldSaveService{}.save_world(state, snapshot));
        }));
        ASSERT_FALSE(fs::exists(P::WorldSaveService::active_regions_path(snapshot)));
        ASSERT_TRUE(fs::exists(P::WorldSaveService::world_manifest_path(snapshot)));
        reader.GetWorldSystem()->SetWaterSimWindowCursor(19);
        auto before = SavedPresetTestDiskBytes(snapshot);
        ASSERT_TRUE(reader.LoadWorldStateFrom(snapshot)) << reader.GetWorldOpenError();
        EXPECT_EQ(reader.GetWorldSystem()->GetWaterSimWindowCursor(), saved_cursor);
        EXPECT_EQ(SavedPresetTestDiskBytes(snapshot), before);

        auto metadata = nlohmann::json::parse(ReadClockTestFile(snapshot / "world_info.json"));
        ASSERT_EQ(metadata.erase("waterSimCursor"), 1u);
        ASSERT_TRUE(P::WorldSaveService::save_metadata(metadata.dump(), snapshot));
        reader.GetWorldSystem()->SetWaterSimWindowCursor(19);
        before = SavedPresetTestDiskBytes(snapshot);
        ASSERT_TRUE(reader.LoadWorldStateFrom(snapshot)) << reader.GetWorldOpenError();
        EXPECT_EQ(reader.GetWorldSystem()->GetWaterSimWindowCursor(), 0u);
        EXPECT_EQ(SavedPresetTestDiskBytes(snapshot), before);

        ASSERT_TRUE(P::WorldSaveService::remove_member(snapshot, "world_info.json"));
        ASSERT_FALSE(fs::exists(snapshot / "world_info.json"));
        ASSERT_TRUE(fs::exists(P::WorldSaveService::world_manifest_path(snapshot)));
        reader.GetWorldSystem()->SetWaterSimWindowCursor(19);
        before = SavedPresetTestDiskBytes(snapshot);
        ASSERT_TRUE(reader.LoadWorldStateFrom(snapshot)) << reader.GetWorldOpenError();
        EXPECT_EQ(reader.GetWorldSystem()->GetWaterSimWindowCursor(), 0u);
        EXPECT_EQ(SavedPresetTestDiskBytes(snapshot), before);
    }
    reader.GetWorldSystem()->SetWaterSimWindowCursor(19);
    ASSERT_TRUE(reader.LoadWorldStateFrom(Root().path() / "missing-water-snapshot"));
    EXPECT_EQ(reader.GetWorldSystem()->GetWaterSimWindowCursor(), 0u);
}

TEST_P(SavedWorldPresetTest, InvalidWaterCursorRefusesCatalogAndSnapshotWithoutChangingCursor) {
    GameSession source;
    Configure(source);
    ASSERT_TRUE(source.CreateWorld("Invalid water cursor", "1337", "default"));
    const auto save = source.GetWorldSaveDir();
    const auto metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    const std::array<nlohmann::json, 8> invalid{
        nullptr, -1, 1.5, 1e38, true, "65", nlohmann::json::array(), nlohmann::json::object()};
    for (const auto& value : invalid) {
        SCOPED_TRACE(value.dump());
        auto malformed = metadata;
        malformed["waterSimCursor"] = value;
        ASSERT_NO_FATAL_FAILURE(WriteSavedPresetTestJson(save / "world_info.json", malformed));
        const auto before = SavedPresetTestDiskBytes(save);
        GameSession reader;
        Configure(reader);
        ASSERT_TRUE(reader.CreateTransientWorld("Cursor refusal", "1337", "default"));
        reader.GetWorldSystem()->SetWaterSimWindowCursor(19);
        EXPECT_FALSE(reader.LoadWorldStateFrom(save));
        EXPECT_NE(reader.GetWorldOpenError().find("water simulation cursor"), std::string::npos);
        EXPECT_EQ(reader.GetWorldSystem()->GetWaterSimWindowCursor(), 19u);
        EXPECT_FALSE(
            P::InspectSavedWorld(Root().path(), source.GetMetadata().worldId).error.empty());
        EXPECT_EQ(SavedPresetTestDiskBytes(save), before);
    }
}

TEST_P(SavedWorldPresetTest, UnmarkedInvalidDestinationRefusesBeforePresetOrPayloadWrites) {
    GameSession session;
    ASSERT_NO_FATAL_FAILURE(CreateSnapshotSession(session));
    std::shared_ptr<Luminumbra::Chunk> chunk;
    ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(session, chunk, false));
    const PendingSnapshotState pending(session);
    const auto save = session.GetWorldSaveDir();
    const std::array<std::pair<fs::path, std::string>, 5> artifacts{{
        {P::WorldSaveService::world_state_path(save), "obsolete snapshot"},
        {P::WorldSaveService::region_file_path(save, 0, 0), std::string("LMR1\xff\xff\0\0", 8)},
        {P::WorldSaveService::region_file_path(save, 0, 0), "LMR1"},
        {P::WorldSaveService::world_manifest_path(save), "{"},
        {P::WorldSaveService::region_directory(save) / "unknown.bin", "unknown artifact"},
    }};
    for (const auto& [path, bytes] : artifacts) {
        SCOPED_TRACE(path.generic_string());
        fs::remove_all(save);
        fs::create_directories(path.parent_path());
        {
            std::ofstream output(path, std::ios::binary);
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            output.close();
            ASSERT_FALSE(output.fail());
        }
        ASSERT_FALSE(fs::exists(save / "preset.json"));
        ASSERT_FALSE(fs::exists(save / "world_info.json"));
        ASSERT_FALSE(P::WorldSaveService::validate_save(save));
        const SnapshotDestinationBytes before(save);
        EXPECT_FALSE(session.SaveWorld());
        before.ExpectUnchanged();
        pending.ExpectUnchanged(session, *chunk);
        EXPECT_FALSE(session.SaveWorldState());
        before.ExpectUnchanged();
        pending.ExpectUnchanged(session, *chunk);
        EXPECT_FALSE(session.SaveWorldStateTo(save));
        before.ExpectUnchanged();
        pending.ExpectUnchanged(session, *chunk);
        EXPECT_FALSE(fs::exists(save / "preset.json"));
        EXPECT_FALSE(fs::exists(save / "world_info.json"));
    }
}

INSTANTIATE_TEST_SUITE_P(ActiveRegionsEnabledAndDisabled, SavedWorldPresetTest, testing::Bool());

TEST(GameSessionHeadlessWorldTest,
     TransientCalendarSnapshotsPreservePresetIdentityAndRefuseChanges) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original;
    original.SetRootPath(root.root_string());
    original.SetJobSystem(&jobs);
    original.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(original.CreateWorld("Snapshot identity", "1337", "default"));
    const auto original_metadata =
        nlohmann::json::parse(ReadClockTestFile(original.GetWorldSaveDir() / "world_info.json"));

    for (const bool dirty : {false, true}) {
        SCOPED_TRACE(dirty);
        GameSession preview;
        preview.SetRootPath(root.root_string());
        preview.SetJobSystem(&jobs);
        preview.SetActiveRegionsEnabled(false);
        const SnapshotDestinationBytes before_preview(root.path());
        ASSERT_TRUE(preview.CreateTransientWorld("Calendar preview", "1337", "default"));
        before_preview.ExpectUnchanged();
        EXPECT_EQ(preview.PresetContentHash(), original.PresetContentHash());
        const auto snapshot = root.path() / (dirty ? "dirty-preview" : "empty-preview");
        ASSERT_TRUE(preview.SaveWorldStateTo(snapshot));
        const auto metadata =
            nlohmann::json::parse(ReadClockTestFile(snapshot / "world_info.json"));
        EXPECT_EQ(metadata.at("renderCalendar"), original_metadata.at("renderCalendar"));
        EXPECT_EQ(metadata.at("presetFormat"), original_metadata.at("presetFormat"));
        EXPECT_EQ(metadata.at("presetIdentity"), original_metadata.at("presetIdentity"));
        EXPECT_EQ(metadata.at("presetIdentity"),
                  P::StableChecksum(
                      nlohmann::json::parse(ReadClockTestFile(snapshot / "preset.json")).dump()));
        const SnapshotPresetFiles preset_files(snapshot);
        auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
        chunk->set_state(Luminumbra::ChunkState::Idle);
        if (dirty) {
            chunk->mark_voxel_data_dirty();
            ASSERT_TRUE(preview.GetWorldSystem()->adopt_streamed_chunk(chunk));
        }
        for (const auto* changed : {"preset", "identity", "both"}) {
            SCOPED_TRACE(changed);
            ASSERT_NO_FATAL_FAILURE(preset_files.Rewrite(changed));
            const SnapshotDestinationBytes before(root.path());
            EXPECT_FALSE(preview.SaveWorldStateTo(snapshot));
            before.ExpectUnchanged();
            EXPECT_EQ(chunk->is_voxel_data_dirty(), dirty);
            ASSERT_NO_FATAL_FAILURE(preset_files.Restore());
        }
        ASSERT_TRUE(preview.SaveWorldStateTo(snapshot));
        EXPECT_FALSE(chunk->is_voxel_data_dirty());
    }
}

// Normalize historical text-mode energy records and current durable binary
// records for content assertions. Exact-byte assertions use ReadClockTestFile.
std::string ReadClockTestTextRecord(const fs::path& path) {
    std::string text = ReadClockTestFile(path);
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

nlohmann::json SessionWorldSubHashes(GameSession& session) {
    Luminumbra::WorldStreamingState state;
    for (const auto& chunk : session.GetWorldSystem()->snapshot_streamed_chunks())
        state.insert_chunk(chunk);
    std::string aether = session.GetAetherFieldSystem()->ComputeAetherSubHash();
    const auto energy = session.ComputeAetherStateSubHash();
    if (!energy.empty())
        aether = P::StableChecksum(aether + "|state:" + energy);
    return {{"chunk",
             session.ActiveRegionsEnabled()
                 ? session.ComputeChunkStateHash()
                 : P::ComputeWorldStreamingStateHash(
                       state, session.GetWorldSystem()->GetWaterSimWindowCursor())},
            {"wind", session.GetWindFieldSystem()->ComputeWindSubHash()},
            {"weather", session.GetWeatherSystem()->ComputeWeatherSubHash()},
            {"aether", aether},
            {"scents", session.ComputeScentSubHash()},
            {"ecology",
             session.FoldClockIntoEcologyHash(
                 luminumbra::ai::ComputeEcologySubHash(session.GetRegistry()))},
            {"plants", session.ComputePlantSubHash()}};
}

std::string SessionWorldHash(GameSession& session) {
    const auto hashes = SessionWorldSubHashes(session);
    return P::ComposeWorldHash(hashes.at("chunk"),
                               hashes.at("wind"),
                               hashes.at("weather"),
                               hashes.at("aether"),
                               hashes.at("scents"),
                               hashes.at("ecology"),
                               hashes.at("plants"));
}

entt::entity ClockTestPlant(entt::registry& registry, const Luminumbra::Vec3& position) {
    const auto plant = registry.create();
    registry.emplace<C::PlantTag>(plant);
    registry.emplace<C::PlantGenomeComponent>(plant).genes.fill(0.5f);
    registry.emplace<C::PlantGrowthComponent>(plant);
    registry.emplace<C::TransformComponent>(plant).position = position;
    return plant;
}

entt::entity ClockTestCreature(entt::registry& registry) {
    const auto creature = registry.create();
    registry.emplace<C::CreatureComponent>(creature);
    registry.emplace<C::CircadianComponent>(creature);
    registry.emplace<C::MigratoryComponent>(creature);
    registry.emplace<C::TransformComponent>(creature).position = Luminumbra::Vec3(0.0f);
    registry.emplace<C::InstinctAgentComponent>(creature).actor_id = "clock-observer";
    registry.emplace<C::NeedsComponent>(creature).needs = {{"day", 0.0f, 0.0f},
                                                           {"year", 0.0f, 0.0f}};
    registry.emplace<C::StimulusSubscriptionComponent>(creature).subscriptions = {
        {luminumbra::ai::StimulusChannel::TimeOfDay, "day", 0.001f},
        {luminumbra::ai::StimulusChannel::Season, "year", 0.001f}};
    return creature;
}

void CommitImportedClockMetadata(const fs::path& save, const std::string& bytes) {
    P::SaveTransaction imported(save, 0, {}, &P::WorldSaveService::member_digest);
    ASSERT_TRUE(imported.begin(nullptr));
    ASSERT_TRUE(imported.protect({"world_info.json"}, nullptr));
    ASSERT_TRUE(P::WorldSaveService::write_member_bytes(save, "world_info.json", bytes));
    ASSERT_TRUE(imported.commit(nullptr));
}

TEST(GameSessionHeadlessWorldTest, ActiveClockSaveLoadContinuationMatchesUninterruptedWorldHash) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original, control, loaded, absent_energy, wrong_clock;
    for (auto* session : {&original, &control, &loaded, &absent_energy, &wrong_clock}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetActiveRegionsEnabled(true);
        session->SetAetherStateEnabled(true);
    }
    ASSERT_TRUE(original.CreateWorld("Clock saved", "1337", "default"));
    ASSERT_TRUE(control.CreateTransientWorld("Clock control", "1337", "default"));
    const auto save = original.GetWorldSaveDir();
    auto metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    const Luminumbra::world::WorldCalendar calendar{120, 4, 1, 2, 8};
    metadata["simulationTick"] = 0u;
    metadata["calendar"] = {{"dayLengthTicks", calendar.dayLengthTicks},
                            {"daysPerYear", calendar.daysPerYear}};
    const auto imported_bytes = metadata.dump();
    ASSERT_NO_FATAL_FAILURE(CommitImportedClockMetadata(save, imported_bytes));
    ASSERT_TRUE(original.LoadWorldStateFrom(save));
    ASSERT_TRUE(control.LoadWorldStateFrom(save));
    EXPECT_EQ(ReadClockTestFile(save / "world_info.json"), imported_bytes);
    for (auto* session : {&original, &control}) {
        ClockTestPlant(session->GetRegistry(), session->GetMetadata().spawnPoint);
        auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
        chunk->set_state(Luminumbra::ChunkState::Idle);
        chunk->mark_voxel_data_dirty();
        ASSERT_TRUE(session->GetWorldSystem()->adopt_streamed_chunk(chunk));
    }
    constexpr int n = 317, k = 73;
    const double dt = original.GetSimulationClock().fixed_dt();
    for (int i = 0; i < n; ++i) {
        ASSERT_EQ(original.TickSimulation(dt), 1u);
        ASSERT_EQ(control.TickSimulation(dt), 1u);
    }
    ASSERT_FALSE(original.GetWeatherSystem()->StormCells().empty());
    // Exercise nonempty energy epoch restoration with a deposit on the last tick.
    for (auto* session : {&original, &control}) {
        session->GetEnergyFieldState()->QueueDeposit(1, 0, 0, 0, 60000);
        ApplySnapshotTestEnergy(*session, n);
    }
    ASSERT_FALSE(original.ComputeAetherStateSubHash().empty());
    const auto before_save = SessionWorldHash(original);
    ASSERT_TRUE(original.SaveWorldState());
    EXPECT_EQ(SessionWorldHash(original), before_save);
    ASSERT_TRUE(loaded.LoadWorld(original.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_EQ(loaded.GetSimulationTickCount(), n);
    EXPECT_EQ(SessionWorldHash(loaded), before_save);
    EXPECT_EQ(loaded.GetWeatherSystem()->ComputeWeatherSubHash(),
              original.GetWeatherSystem()->ComputeWeatherSubHash());
    const auto& saved_storms = original.GetWeatherSystem()->StormCells();
    const auto& loaded_storms = loaded.GetWeatherSystem()->StormCells();
    ASSERT_EQ(saved_storms.size(), loaded_storms.size());
    for (std::size_t i = 0; i < saved_storms.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(saved_storms[i].spawn_tick, loaded_storms[i].spawn_tick);
        EXPECT_EQ(saved_storms[i].center_world, loaded_storms[i].center_world);
        EXPECT_EQ(saved_storms[i].velocity, loaded_storms[i].velocity);
        EXPECT_EQ(saved_storms[i].intensity, loaded_storms[i].intensity);
    }
    const auto missing_save = root.path() / "missing-energy";
    const auto wrong_save = root.path() / "wrong-clock";
    fs::copy(save, missing_save, fs::copy_options::recursive);
    fs::copy(save, wrong_save, fs::copy_options::recursive);
    {
        P::SaveTransaction removed(missing_save, n, {}, &P::WorldSaveService::member_digest);
        ASSERT_TRUE(removed.begin(nullptr));
        ASSERT_TRUE(removed.protect({"aether_state.efs"}, nullptr));
        ASSERT_TRUE(fs::remove(missing_save / "aether_state.efs"));
        ASSERT_TRUE(removed.commit(nullptr));
    }
    metadata = nlohmann::json::parse(ReadClockTestFile(wrong_save / "world_info.json"));
    Luminumbra::world::WorldClock(n + 1, calendar).write_metadata(metadata);
    ASSERT_TRUE(P::WorldSaveService::save_metadata(metadata.dump(), wrong_save));
    ASSERT_TRUE(absent_energy.CreateTransientWorld("Missing energy", "1337", "default"));
    ASSERT_TRUE(wrong_clock.CreateTransientWorld("Wrong clock", "1337", "default"));
    ASSERT_TRUE(absent_energy.LoadWorldStateFrom(missing_save));
    ASSERT_TRUE(wrong_clock.LoadWorldStateFrom(wrong_save));
    EXPECT_NE(SessionWorldHash(absent_energy), before_save);
    EXPECT_NE(SessionWorldHash(wrong_clock), before_save);

    // This slice persists plants and energy, not creature records. Attach the same
    // observing creature to each independently advanced world at the checkpoint.
    std::vector<entt::entity> creatures;
    const std::vector<GameSession*> sessions{
        &original, &control, &loaded, &absent_energy, &wrong_clock};
    creatures.reserve(sessions.size());
    for (auto* session : sessions)
        creatures.push_back(ClockTestCreature(session->GetRegistry()));
    for (int i = 0; i < k; ++i) {
        for (auto* session : sessions) {
            ASSERT_EQ(session->TickSimulation(dt), 1u);
        }
        const auto& reference = control.GetRegistry();
        for (std::size_t j = 0; j < 3; ++j) {
            const auto& registry = sessions[j]->GetRegistry();
            EXPECT_FLOAT_EQ(registry.get<C::CircadianComponent>(creatures[j]).activity,
                            reference.get<C::CircadianComponent>(creatures[1]).activity);
            const auto& actual = registry.get<C::MigratoryComponent>(creatures[j]);
            const auto& expected = reference.get<C::MigratoryComponent>(creatures[1]);
            EXPECT_FLOAT_EQ(actual.drive, expected.drive);
            EXPECT_FLOAT_EQ(actual.wish_x, expected.wish_x);
            EXPECT_FLOAT_EQ(actual.wish_z, expected.wish_z);
            const auto& needs = registry.get<C::NeedsComponent>(creatures[j]).needs;
            const auto& expected_needs = reference.get<C::NeedsComponent>(creatures[1]).needs;
            for (std::size_t need = 0; need < needs.size(); ++need) {
                EXPECT_FLOAT_EQ(needs[need].pressure, expected_needs[need].pressure);
            }
        }
    }
    EXPECT_EQ(loaded.GetSimulationTickCount(), n + k);
    EXPECT_EQ(loaded.GetWorldClock().calendar(), calendar);
    EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(control));
    EXPECT_EQ(SessionWorldHash(original), SessionWorldHash(control));
    EXPECT_NE(SessionWorldHash(absent_energy), SessionWorldHash(control));
    EXPECT_NE(SessionWorldHash(wrong_clock), SessionWorldHash(control));
    EXPECT_NE(wrong_clock.GetRegistry().get<C::CircadianComponent>(creatures[4]).activity,
              control.GetRegistry().get<C::CircadianComponent>(creatures[1]).activity);
    EXPECT_NE(wrong_clock.GetRegistry().get<C::NeedsComponent>(creatures[4]).needs[0].pressure,
              control.GetRegistry().get<C::NeedsComponent>(creatures[1]).needs[0].pressure);
}

void CheckCommittedSnapshotReloadAndSave(const HeadlessRoot& root,
                                         JobSystem& jobs,
                                         const std::string& world_id,
                                         const fs::path& save,
                                         std::uint64_t expected_tick) {
    // answers.md:31 and design.md:60-61 require recovery at world open, before reading members.
    if (P::journal_pending(save)) {
        GameSession recovery;
        recovery.SetRootPath(root.root_string());
        recovery.SetJobSystem(&jobs);
        recovery.SetActiveRegionsEnabled(true);
        ASSERT_TRUE(recovery.LoadWorld(world_id)) << recovery.GetWorldOpenError();
        EXPECT_EQ(recovery.GetSimulationTickCount(), expected_tick);
        EXPECT_FALSE(P::journal_pending(save));
    }
    ASSERT_TRUE(P::WorldSaveService::validate_save(save));
    const auto metadata_bytes = ReadClockTestFile(save / "world_info.json");
    const auto ledger_path = P::WorldSaveService::active_regions_path(save);
    const auto ledger_bytes = ReadClockTestFile(ledger_path);
    Luminumbra::world::ActiveRegionLedger durable;
    ASSERT_TRUE(P::WorldSaveService::load_active_regions(durable, save));
    const auto tick =
        nlohmann::json::parse(metadata_bytes).at("simulationTick").get<std::uint64_t>();
    ASSERT_EQ(tick, expected_tick);
    ASSERT_EQ(durable.tick(), tick);
    for (const bool edit : {false, true}) {
        SCOPED_TRACE(edit);
        // Each fresh session must load the previous whole commit, including the edit case.
        {
            P::SaveTransaction staged(save, tick, {}, &P::WorldSaveService::member_digest);
            ASSERT_TRUE(staged.begin(nullptr));
            ASSERT_TRUE(
                staged.protect({"world_info.json", "chunks/region/active-regions.arl"}, nullptr));
            ASSERT_TRUE(
                P::WorldSaveService::write_member_bytes(save, "world_info.json", metadata_bytes));
            ASSERT_TRUE(P::WorldSaveService::write_member_bytes(
                save, "chunks/region/active-regions.arl", ledger_bytes));
            ASSERT_TRUE(staged.commit(nullptr));
        }
        GameSession loaded, reloaded;
        for (auto* session : {&loaded, &reloaded}) {
            session->SetRootPath(root.root_string());
            session->SetJobSystem(&jobs);
            session->SetActiveRegionsEnabled(true);
        }
        ASSERT_TRUE(loaded.LoadWorld(world_id)) << loaded.GetWorldOpenError();
        EXPECT_EQ(loaded.GetSimulationTickCount(), tick);
        // Loading the recovered commit preserves its causal clock and canonical ledger.
        EXPECT_EQ(loaded.GetActiveRegionLedger().tick(), durable.tick());
        EXPECT_EQ(loaded.GetActiveRegionLedger().header_tick(), tick);
        EXPECT_EQ(loaded.GetActiveRegionLedger().canonical_bytes(), durable.canonical_bytes());
        EXPECT_EQ(loaded.GetActiveRegionLedger().records(), durable.records());
        EXPECT_EQ(loaded.GetActiveRegionLedger().config(), durable.config());
        EXPECT_EQ(loaded.GetActiveRegionLedger().local_anchor(), durable.local_anchor());
        EXPECT_EQ(loaded.GetRegionSchedule().tick, 0u);
        if (edit) {
            loaded.NotifyGroundObjectEdit(Luminumbra::Vec3(5200, 0, -1500));
            EXPECT_EQ(loaded.GetActiveRegionLedger().records().at({10, -3}).last_edit, tick);
            EXPECT_TRUE(loaded.GetActiveRegionLedger().records().at({10, -3}).wake_pending);
        }
        ASSERT_TRUE(loaded.SaveWorld());
        ASSERT_TRUE(loaded.SaveWorldState());
        EXPECT_EQ(loaded.GetSimulationTickCount(), tick);
        EXPECT_EQ(loaded.GetRegionSchedule().tick, 0u);
        // Only observational save stamps (and the explicit edit) may change.
        auto saved_records = loaded.GetActiveRegionLedger().records();
        if (edit)
            saved_records.erase({10, -3});
        auto expected_records = durable.records();
        for (auto& [key, record] : expected_records) {
            (void)key;
            record.last_save = tick;
        }
        EXPECT_EQ(saved_records, expected_records);
        ASSERT_TRUE(P::WorldSaveService::validate_save(save));
        ASSERT_TRUE(reloaded.LoadWorld(world_id)) << reloaded.GetWorldOpenError();
        EXPECT_EQ(reloaded.GetSimulationTickCount(), tick);
        EXPECT_EQ(reloaded.GetActiveRegionLedger().encode(),
                  loaded.GetActiveRegionLedger().encode());
        EXPECT_EQ(SessionWorldHash(reloaded), SessionWorldHash(loaded));
    }
}

void CheckPartialPairReloadAndSave(const HeadlessRoot& root,
                                   JobSystem& jobs,
                                   const std::string& world_id,
                                   const fs::path& save) {
    const auto metadata_bytes = ReadClockTestFile(save / "world_info.json");
    const auto ledger_path = P::WorldSaveService::active_regions_path(save);
    const auto ledger_bytes = ReadClockTestFile(ledger_path);
    Luminumbra::world::ActiveRegionLedger durable;
    ASSERT_TRUE(P::WorldSaveService::load_active_regions(durable, save));
    const auto tick =
        nlohmann::json::parse(metadata_bytes).at("simulationTick").get<std::uint64_t>();
    ASSERT_LT(durable.tick(), tick);
    for (const bool edit : {false, true}) {
        SCOPED_TRACE(edit);
        // Each fresh session must load the partial pair, including the edit case.
        {
            P::SaveTransaction transaction(save, tick, {}, &P::WorldSaveService::member_digest);
            ASSERT_TRUE(transaction.begin(nullptr));
            ASSERT_TRUE(transaction.protect({"world_info.json", "chunks/region/active-regions.arl"},
                                            nullptr));
            std::ofstream(save / "world_info.json", std::ios::binary) << metadata_bytes;
            std::ofstream(ledger_path, std::ios::binary) << ledger_bytes;
            ASSERT_TRUE(transaction.commit(nullptr));
        }
        GameSession loaded, reloaded;
        for (auto* session : {&loaded, &reloaded}) {
            session->SetRootPath(root.root_string());
            session->SetJobSystem(&jobs);
            session->SetActiveRegionsEnabled(true);
        }
        ASSERT_TRUE(loaded.LoadWorld(world_id)) << loaded.GetWorldOpenError();
        EXPECT_EQ(loaded.GetSimulationTickCount(), tick);
        // Reconciliation raises only the validation ceiling. The canonical tick
        // stays where the interrupted save left it, so recovery cannot move the
        // hash projection of an accepted state.
        EXPECT_EQ(loaded.GetActiveRegionLedger().tick(), durable.tick());
        EXPECT_EQ(loaded.GetActiveRegionLedger().header_tick(), tick);
        EXPECT_EQ(loaded.GetActiveRegionLedger().canonical_bytes(), durable.canonical_bytes());
        EXPECT_EQ(loaded.GetActiveRegionLedger().records(), durable.records());
        EXPECT_EQ(loaded.GetActiveRegionLedger().config(), durable.config());
        EXPECT_EQ(loaded.GetActiveRegionLedger().local_anchor(), durable.local_anchor());
        EXPECT_EQ(loaded.GetRegionSchedule().tick, 0u);
        if (edit) {
            loaded.NotifyGroundObjectEdit(Luminumbra::Vec3(5200, 0, -1500));
            EXPECT_EQ(loaded.GetActiveRegionLedger().records().at({10, -3}).last_edit, tick);
            EXPECT_TRUE(loaded.GetActiveRegionLedger().records().at({10, -3}).wake_pending);
        }
        ASSERT_TRUE(loaded.SaveWorld());
        ASSERT_TRUE(loaded.SaveWorldState());
        EXPECT_EQ(loaded.GetSimulationTickCount(), tick);
        EXPECT_EQ(loaded.GetRegionSchedule().tick, 0u);
        // Only observational save stamps (and the explicit edit) may change.
        auto saved_records = loaded.GetActiveRegionLedger().records();
        if (edit)
            saved_records.erase({10, -3});
        auto expected_records = durable.records();
        for (auto& [key, record] : expected_records) {
            (void)key;
            record.last_save = tick;
        }
        EXPECT_EQ(saved_records, expected_records);
        ASSERT_TRUE(P::WorldSaveService::validate_save(save));
        ASSERT_TRUE(reloaded.LoadWorld(world_id)) << reloaded.GetWorldOpenError();
        EXPECT_EQ(reloaded.GetSimulationTickCount(), tick);
        EXPECT_EQ(reloaded.GetActiveRegionLedger().encode(),
                  loaded.GetActiveRegionLedger().encode());
        EXPECT_EQ(SessionWorldHash(reloaded), SessionWorldHash(loaded));
    }
}

TEST(GameSessionHeadlessWorldTest,
     ActiveClockWritesMetadataWithoutDirtyChunksAndExplicitLoadsRestoreIt) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Empty clock", "1337", "default"));
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    const auto alternate = root.path() / "explicit-snapshot";
    ASSERT_TRUE(session.SaveWorldStateTo(alternate));
    const auto metadata = nlohmann::json::parse(ReadClockTestFile(alternate / "world_info.json"));
    EXPECT_EQ(metadata.at("presetFormat"), 1);
    EXPECT_EQ(metadata.at("presetIdentity"),
              P::StableChecksum(
                  nlohmann::json::parse(ReadClockTestFile(alternate / "preset.json")).dump()));
    EXPECT_EQ(nlohmann::json::parse(ReadClockTestFile(alternate / "preset.json")),
              nlohmann::json::parse(ReadClockTestFile(session.GetWorldSaveDir() / "preset.json")));
    EXPECT_EQ(metadata.at("simulationTick"), 1u);
    EXPECT_TRUE(fs::exists(P::WorldSaveService::active_regions_path(alternate)));
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_TRUE(session.SaveWorld());
    const auto save = session.GetWorldSaveDir();
    Luminumbra::world::ActiveRegionLedger saved_ledger;
    ASSERT_TRUE(P::WorldSaveService::load_active_regions(saved_ledger, save));
    ASSERT_EQ(saved_ledger.tick(), 2u);
    ASSERT_EQ(P::InspectSavedWorld(root.path(), session.GetMetadata().worldId).clock.tick(), 2u);
    ASSERT_TRUE(session.LoadWorldStateFrom(alternate));
    EXPECT_EQ(session.GetSimulationTickCount(), 1u);
    EXPECT_EQ(session.GetWorldClock().calendar(), Luminumbra::world::WorldCalendar{});
    const auto restored_ledger = session.GetActiveRegionLedger().canonical_bytes();
    const auto newer_metadata = ReadClockTestFile(save / "world_info.json");
    const auto newer_ledger = ReadClockTestFile(P::WorldSaveService::active_regions_path(save));
    auto invalid_metadata = metadata;
    invalid_metadata["simulationTick"] = 3u;
    auto incoming_ledger = session.GetActiveRegionLedger();
    EXPECT_FALSE(P::WorldSaveService::save_metadata_and_active_regions(
        invalid_metadata.dump(), incoming_ledger, save));
    EXPECT_EQ(ReadClockTestFile(save / "world_info.json"), newer_metadata);
    EXPECT_EQ(ReadClockTestFile(P::WorldSaveService::active_regions_path(save)), newer_ledger);
    ASSERT_TRUE(session.SaveWorld());
    const auto inspected = P::InspectSavedWorld(root.path(), session.GetMetadata().worldId);
    EXPECT_TRUE(inspected.error.empty());
    EXPECT_EQ(inspected.clock.tick(), 1u);
    ASSERT_TRUE(P::WorldSaveService::validate_save(save));
    ASSERT_TRUE(P::WorldSaveService::load_active_regions(saved_ledger, save));
    EXPECT_EQ(saved_ledger.tick(), 1u);
    EXPECT_EQ(saved_ledger.canonical_bytes(), restored_ledger);
    const auto restored_hash = SessionWorldHash(session);
    ASSERT_TRUE(session.LoadWorld(session.GetMetadata().worldId)) << session.GetWorldOpenError();
    EXPECT_EQ(session.GetSimulationTickCount(), 1u);
    EXPECT_EQ(session.GetActiveRegionLedger().canonical_bytes(), restored_ledger);
    EXPECT_EQ(SessionWorldHash(session), restored_hash);
    ASSERT_TRUE(session.SaveWorldState());
    EXPECT_TRUE(P::WorldSaveService::validate_save(save));

    // Independently committed members retain devel's readable partial-pair contract.
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_TRUE(session.SaveWorld());
    ASSERT_TRUE(session.LoadWorldStateFrom(alternate));
    auto older_ledger = session.GetActiveRegionLedger();
    ASSERT_TRUE(
        P::WorldSaveService::save_active_regions(older_ledger, session.GetWorldClock(), save));
    P::WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    const bool interrupted_save = P::WorldSaveService::save_metadata(metadata.dump(), save);
    P::WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
    EXPECT_FALSE(interrupted_save);
    ASSERT_TRUE(P::WorldSaveService::validate_save(save));
    EXPECT_EQ(P::InspectSavedWorld(root.path(), session.GetMetadata().worldId).clock.tick(), 2u);
    ASSERT_TRUE(P::WorldSaveService::load_active_regions(saved_ledger, save));
    EXPECT_EQ(saved_ledger.tick(), 1u);
    ASSERT_NO_FATAL_FAILURE(
        CheckPartialPairReloadAndSave(root, jobs, session.GetMetadata().worldId, save));
    ASSERT_TRUE(session.SaveWorld());
    ASSERT_TRUE(session.LoadWorld(session.GetMetadata().worldId)) << session.GetWorldOpenError();
    EXPECT_EQ(session.GetSimulationTickCount(), 1u);
    EXPECT_EQ(session.GetActiveRegionLedger().canonical_bytes(), restored_ledger);
    EXPECT_EQ(SessionWorldHash(session), restored_hash);
}

TEST(GameSessionHeadlessWorldTest, InterruptedSessionRewindRestoresCommittedSnapshot) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Empty clock", "1337", "default"));
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    const auto alternate = root.path() / "explicit-snapshot";
    ASSERT_TRUE(session.SaveWorldStateTo(alternate));
    const auto metadata = nlohmann::json::parse(ReadClockTestFile(alternate / "world_info.json"));
    EXPECT_EQ(metadata.at("presetFormat"), 1);
    EXPECT_EQ(metadata.at("presetIdentity"),
              P::StableChecksum(
                  nlohmann::json::parse(ReadClockTestFile(alternate / "preset.json")).dump()));
    EXPECT_EQ(nlohmann::json::parse(ReadClockTestFile(alternate / "preset.json")),
              nlohmann::json::parse(ReadClockTestFile(session.GetWorldSaveDir() / "preset.json")));
    EXPECT_EQ(metadata.at("simulationTick"), 1u);
    EXPECT_TRUE(fs::exists(P::WorldSaveService::active_regions_path(alternate)));
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_TRUE(session.SaveWorld());
    const auto save = session.GetWorldSaveDir();
    Luminumbra::world::ActiveRegionLedger saved_ledger;
    ASSERT_TRUE(P::WorldSaveService::load_active_regions(saved_ledger, save));
    ASSERT_EQ(saved_ledger.tick(), 2u);
    ASSERT_EQ(P::InspectSavedWorld(root.path(), session.GetMetadata().worldId).clock.tick(), 2u);
    ASSERT_TRUE(session.LoadWorldStateFrom(alternate));
    EXPECT_EQ(session.GetSimulationTickCount(), 1u);
    EXPECT_EQ(session.GetWorldClock().calendar(), Luminumbra::world::WorldCalendar{});
    const auto restored_ledger = session.GetActiveRegionLedger().canonical_bytes();
    const auto newer_metadata = ReadClockTestFile(save / "world_info.json");
    const auto newer_ledger = ReadClockTestFile(P::WorldSaveService::active_regions_path(save));
    auto invalid_metadata = metadata;
    invalid_metadata["simulationTick"] = 3u;
    auto incoming_ledger = session.GetActiveRegionLedger();
    EXPECT_FALSE(P::WorldSaveService::save_metadata_and_active_regions(
        invalid_metadata.dump(), incoming_ledger, save));
    EXPECT_EQ(ReadClockTestFile(save / "world_info.json"), newer_metadata);
    EXPECT_EQ(ReadClockTestFile(P::WorldSaveService::active_regions_path(save)), newer_ledger);
    ASSERT_TRUE(session.SaveWorld());
    const auto inspected = P::InspectSavedWorld(root.path(), session.GetMetadata().worldId);
    EXPECT_TRUE(inspected.error.empty());
    EXPECT_EQ(inspected.clock.tick(), 1u);
    ASSERT_TRUE(P::WorldSaveService::validate_save(save));
    ASSERT_TRUE(P::WorldSaveService::load_active_regions(saved_ledger, save));
    EXPECT_EQ(saved_ledger.tick(), 1u);
    EXPECT_EQ(saved_ledger.canonical_bytes(), restored_ledger);
    const auto restored_hash = SessionWorldHash(session);
    ASSERT_TRUE(session.LoadWorld(session.GetMetadata().worldId)) << session.GetWorldOpenError();
    EXPECT_EQ(session.GetSimulationTickCount(), 1u);
    EXPECT_EQ(session.GetActiveRegionLedger().canonical_bytes(), restored_ledger);
    EXPECT_EQ(SessionWorldHash(session), restored_hash);
    ASSERT_TRUE(session.SaveWorldState());
    EXPECT_TRUE(P::WorldSaveService::validate_save(save));

    // design.md:60-61: a failure between rewind replacements restores the whole previous commit.
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_TRUE(session.SaveWorld());
    const auto before_rewind_metadata = ReadClockTestFile(save / "world_info.json");
    const auto before_rewind_ledger =
        ReadClockTestFile(P::WorldSaveService::active_regions_path(save));
    const auto before_rewind_index = ReadClockTestFile(P::snapshot_index_path(save));
    ASSERT_TRUE(session.LoadWorldStateFrom(alternate));
    P::WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    const bool interrupted_save = session.SaveWorld();
    P::WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
    EXPECT_FALSE(interrupted_save);
    ASSERT_TRUE(P::WorldSaveService::validate_save(save));
    EXPECT_EQ(P::InspectSavedWorld(root.path(), session.GetMetadata().worldId).clock.tick(), 2u);
    ASSERT_TRUE(P::WorldSaveService::load_active_regions(saved_ledger, save));
    EXPECT_EQ(saved_ledger.tick(), 2u);
    EXPECT_EQ(ReadClockTestFile(save / "world_info.json"), before_rewind_metadata);
    EXPECT_EQ(ReadClockTestFile(P::WorldSaveService::active_regions_path(save)),
              before_rewind_ledger);
    EXPECT_EQ(ReadClockTestFile(P::snapshot_index_path(save)), before_rewind_index);
    ASSERT_NO_FATAL_FAILURE(
        CheckCommittedSnapshotReloadAndSave(root, jobs, session.GetMetadata().worldId, save, 2));
    ASSERT_TRUE(session.SaveWorld());
    ASSERT_TRUE(session.LoadWorld(session.GetMetadata().worldId)) << session.GetWorldOpenError();
    EXPECT_EQ(session.GetSimulationTickCount(), 1u);
    EXPECT_EQ(session.GetActiveRegionLedger().canonical_bytes(), restored_ledger);
    EXPECT_EQ(SessionWorldHash(session), restored_hash);
}

TEST(GameSessionHeadlessWorldTest, FailedForwardLedgerWriteReloadsAndSavesImmediately) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Forward failure", "1337", "default"));
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_TRUE(session.SaveWorld());
    const auto directory = session.GetWorldSaveDir();
    const auto previous_ledger =
        ReadClockTestFile(P::WorldSaveService::active_regions_path(directory));
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    P::WorldSaveService::set_before_active_regions_replace_for_testing([] { return false; });
    // Independently published members can still contain an older ledger. Session
    // saves below additionally test rollback of the complete indexed snapshot.
    auto metadata = nlohmann::json::parse(ReadClockTestFile(directory / "world_info.json"));
    session.GetWorldClock().write_metadata(metadata);
    ASSERT_TRUE(P::WorldSaveService::save_metadata(metadata.dump(), directory));
    auto ledger = session.GetActiveRegionLedger();
    const bool saved =
        P::WorldSaveService::save_active_regions(ledger, session.GetWorldClock(), directory);
    P::WorldSaveService::set_before_active_regions_replace_for_testing(nullptr);
    ASSERT_FALSE(saved);
    EXPECT_EQ(nlohmann::json::parse(ReadClockTestFile(directory / "world_info.json"))
                  .at("simulationTick"),
              2u);
    EXPECT_EQ(ReadClockTestFile(P::WorldSaveService::active_regions_path(directory)),
              previous_ledger);
    ASSERT_NO_FATAL_FAILURE(CheckPartialPairReloadAndSave(
        root, jobs, session.GetMetadata().worldId, session.GetWorldSaveDir()));
}

TEST(GameSessionHeadlessWorldTest, FailedForwardSessionSaveRestoresCommittedSnapshot) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Forward failure", "1337", "default"));
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_TRUE(session.SaveWorld());
    const auto save = session.GetWorldSaveDir();
    const auto before_metadata = ReadClockTestFile(save / "world_info.json");
    const auto before_ledger = ReadClockTestFile(P::WorldSaveService::active_regions_path(save));
    const auto before_index = ReadClockTestFile(P::snapshot_index_path(save));
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    P::WorldSaveService::set_before_active_regions_replace_for_testing([] { return false; });
    const bool saved = session.SaveWorld();
    P::WorldSaveService::set_before_active_regions_replace_for_testing(nullptr);
    ASSERT_FALSE(saved);
    // design.md:60-61: failed forward writes also restore the whole previous commit.
    EXPECT_EQ(ReadClockTestFile(save / "world_info.json"), before_metadata);
    EXPECT_EQ(ReadClockTestFile(P::WorldSaveService::active_regions_path(save)), before_ledger);
    EXPECT_EQ(ReadClockTestFile(P::snapshot_index_path(save)), before_index);
    ASSERT_NO_FATAL_FAILURE(
        CheckCommittedSnapshotReloadAndSave(root, jobs, session.GetMetadata().worldId, save, 1));
}

#if GTEST_HAS_DEATH_TEST && !defined(_WIN32)
TEST(GameSessionHeadlessWorldDeathTest, TerminatedLedgerStagingLeavesLoadableAndSavableWorld) {
    // Fork before starting any workers, so the child owns every job it must drain.
    // Fast death-test mode also preserves this unique directory in the child.
    GTEST_FLAG_SET(death_test_style, "fast");
    const HeadlessRoot root;
    ASSERT_EXIT(
        {
            JobSystem jobs;
            jobs.startup(1);
            GameSession session;
            session.SetRootPath(root.root_string());
            session.SetJobSystem(&jobs);
            session.SetActiveRegionsEnabled(true);
            if (!session.CreateWorld("Ledger crash", "1337", "default"))
                std::_Exit(1);
            session.TickSimulation(session.GetSimulationClock().fixed_dt());
            if (!session.SaveWorld())
                std::_Exit(2);
            session.TickSimulation(session.GetSimulationClock().fixed_dt());
            auto metadata = nlohmann::json::parse(
                ReadClockTestFile(session.GetWorldSaveDir() / "world_info.json"));
            session.GetWorldClock().write_metadata(metadata);
            if (!P::WorldSaveService::save_metadata(metadata.dump(), session.GetWorldSaveDir()))
                std::_Exit(4);
            P::WorldSaveService::set_before_active_regions_replace_for_testing([]() -> bool {
                std::_Exit(73); // no destructors, unwinding or temporary-file cleanup
            });
            auto ledger = session.GetActiveRegionLedger();
            P::WorldSaveService::save_active_regions(
                ledger, session.GetWorldClock(), session.GetWorldSaveDir());
            std::_Exit(3);
        },
        testing::ExitedWithCode(73),
        "");
    const auto catalog = P::EnumerateSavedWorlds(root.path());
    ASSERT_TRUE(catalog.error.empty()) << catalog.error;
    ASSERT_EQ(catalog.worlds.size(), 1u);
    const auto save = root.path() / "worlds/saves" / catalog.worlds.front().metadata.worldId;
    std::vector<fs::path> debris;
    for (const auto& entry : fs::recursive_directory_iterator(save)) {
        if (entry.path().filename().string().find("active-regions.arl.tmp.") == 0 &&
            entry.path().extension() != ".owner")
            debris.push_back(entry.path());
    }
    ASSERT_EQ(debris.size(), 1u);
    EXPECT_EQ(debris.front().parent_path(), save);
    auto ownership = debris.front();
    ownership += ".owner";
    ASSERT_TRUE(fs::is_regular_file(ownership));
    EXPECT_EQ(
        nlohmann::json::parse(ReadClockTestFile(save / "world_info.json")).at("simulationTick"),
        2u);
    Luminumbra::world::ActiveRegionLedger staged;
    std::string error;
    ASSERT_TRUE(Luminumbra::world::ActiveRegionLedger::decode(
        ReadClockTestFile(debris.front()), staged, error))
        << error;
    EXPECT_EQ(staged.tick(), 2u);
    EXPECT_TRUE(P::journal_pending(save));
    ASSERT_NE(P::recover_pending_journal(save, nullptr), P::RecoveryOutcome::Failed);
    ASSERT_TRUE(P::WorldSaveService::validate_save(save));
    JobSystem jobs;
    jobs.startup(1);
    ASSERT_NO_FATAL_FAILURE(
        CheckPartialPairReloadAndSave(root, jobs, save.filename().string(), save));
    // Recovery never depends on cleaning the staging file up, but the next
    // successful save must reclaim it. Otherwise every interruption leaks a full
    // ledger copy and repeated crashes exhaust the volume.
    std::size_t remaining = 0;
    for (const auto& entry : fs::recursive_directory_iterator(save))
        if (entry.path().filename().string().find("active-regions.arl.tmp.") == 0)
            ++remaining;
    EXPECT_EQ(remaining, 0u);
    EXPECT_FALSE(fs::exists(debris.front()));
    EXPECT_FALSE(fs::exists(ownership));
}
#endif

TEST(GameSessionHeadlessWorldTest, ActiveClockKeepsAmbientAnchorWhenClientSavesMovedSpawn) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original, control, loaded, explicit_load, wrong_anchor;
    for (auto* session : {&original, &control, &loaded, &explicit_load, &wrong_anchor}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetActiveRegionsEnabled(true);
    }
    ASSERT_TRUE(original.CreateWorld("Moving spawn", "1337", "default"));
    ASSERT_TRUE(control.CreateTransientWorld("Control", "1337", "default"));
    const auto anchor = original.GetMetadata().spawnPoint;
    for (auto* session : {&original, &control})
        ClockTestPlant(session->GetRegistry(), anchor);
    const double dt = original.GetSimulationClock().fixed_dt();
    constexpr int n = 317, k = 73;
    for (int i = 0; i < n; ++i) {
        ASSERT_EQ(original.TickSimulation(dt), 1u);
        ASSERT_EQ(control.TickSimulation(dt), 1u);
    }
    ASSERT_FALSE(original.GetWeatherSystem()->StormCells().empty());
    const auto before = SessionWorldHash(original);
    const auto spawn = anchor + Luminumbra::Vec3(2400.0f, 10.0f, -1200.0f);
    // The production quit/shutdown path changes the respawn position before saving.
    original.SetSpawnPoint(spawn);
    control.SetSpawnPoint(spawn);
    ASSERT_TRUE(original.SaveWorldState());
    ASSERT_TRUE(original.SaveWorld());
    EXPECT_EQ(SessionWorldHash(original), before);
    const auto save = original.GetWorldSaveDir();
    auto metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    EXPECT_EQ(metadata.at("spawnPoint").at("x"), spawn.x);
    EXPECT_EQ(metadata.at("ambientFieldAnchor").at("x"), anchor.x);
    EXPECT_EQ(metadata.at("ambientFieldAnchor").at("y"), anchor.y);
    EXPECT_EQ(metadata.at("ambientFieldAnchor").at("z"), anchor.z);
    ASSERT_TRUE(loaded.LoadWorld(original.GetMetadata().worldId));
    EXPECT_EQ(loaded.GetMetadata().spawnPoint, spawn);
    const auto explicit_save = root.path() / "moved-spawn-snapshot";
    ASSERT_TRUE(original.SaveWorldStateTo(explicit_save));
    ASSERT_TRUE(explicit_load.CreateTransientWorld("Explicit", "1337", "default"));
    ASSERT_TRUE(explicit_load.LoadWorldStateFrom(explicit_save));
    // Model the bug: using the new respawn position for historical reconstruction.
    metadata["ambientFieldAnchor"] = metadata.at("spawnPoint");
    ASSERT_TRUE(P::WorldSaveService::save_metadata(metadata.dump(), explicit_save));
    ASSERT_TRUE(wrong_anchor.CreateTransientWorld("Wrong anchor", "1337", "default"));
    ASSERT_TRUE(wrong_anchor.LoadWorldStateFrom(explicit_save));
    for (int i = 0; i <= k; ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(SessionWorldHash(original), SessionWorldHash(control));
        EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(control));
        for (auto* session : {&loaded, &explicit_load}) {
            EXPECT_EQ(session->GetWindFieldSystem()->ComputeWindSubHash(),
                      control.GetWindFieldSystem()->ComputeWindSubHash());
            EXPECT_EQ(session->GetWeatherSystem()->ComputeWeatherSubHash(),
                      control.GetWeatherSystem()->ComputeWeatherSubHash());
            EXPECT_EQ(session->GetAetherFieldSystem()->ComputeAetherSubHash(),
                      control.GetAetherFieldSystem()->ComputeAetherSubHash());
            const auto& storms = session->GetWeatherSystem()->StormCells();
            const auto& expected = control.GetWeatherSystem()->StormCells();
            ASSERT_EQ(storms.size(), expected.size());
            for (std::size_t j = 0; j < storms.size(); ++j) {
                EXPECT_EQ(storms[j].spawn_tick, expected[j].spawn_tick);
                EXPECT_EQ(storms[j].center_world, expected[j].center_world);
                EXPECT_EQ(storms[j].velocity, expected[j].velocity);
                EXPECT_EQ(storms[j].intensity, expected[j].intensity);
            }
        }
        EXPECT_NE(wrong_anchor.GetWeatherSystem()->ComputeWeatherSubHash(),
                  control.GetWeatherSystem()->ComputeWeatherSubHash());
        if (i < k) {
            for (auto* session : {&original, &control, &loaded, &explicit_load, &wrong_anchor}) {
                ASSERT_EQ(session->TickSimulation(dt), 1u);
            }
        }
    }
}

TEST(GameSessionHeadlessWorldTest, ClockAnchorMetadataUsesLegacySpawnAndRejectsInvalidAnchors) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Anchor metadata", "1337", "default"));
    const auto save = session.GetWorldSaveDir();
    auto metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    metadata.erase("ambientFieldAnchor");
    metadata["spawnPoint"] = {{"x", 2400.0f}, {"y", 100.0f}, {"z", -1200.0f}};
    ASSERT_TRUE(P::WorldSaveService::save_metadata(metadata.dump(), save));
    ASSERT_TRUE(session.LoadWorldStateFrom(save));
    ASSERT_TRUE(session.SaveWorld());
    metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    EXPECT_EQ(metadata.at("ambientFieldAnchor"),
              (nlohmann::json{{"x", 2400.0f}, {"y", 100.0f}, {"z", -1200.0f}}));
    const auto valid_bytes = ReadClockTestFile(save / "world_info.json");
    const std::vector<nlohmann::json> invalid{nullptr,
                                              true,
                                              "anchor",
                                              {{"x", 0.0f}},
                                              {{"x", 1e100}, {"y", 0.0f}, {"z", 0.0f}},
                                              {{"x", false}, {"y", 0.0f}, {"z", 0.0f}}};
    for (const auto& value : invalid) {
        SCOPED_TRACE(value.dump());
        metadata["ambientFieldAnchor"] = value;
        EXPECT_FALSE(P::WorldSaveService::save_metadata(metadata.dump(), save));
        EXPECT_EQ(ReadClockTestFile(save / "world_info.json"), valid_bytes);
        // Binary mode: the reader compares exact bytes, and Windows text mode would
        // translate newlines and make the comparison fail for the wrong reason.
        std::ofstream(save / "world_info.json", std::ios::binary) << metadata.dump();
        EXPECT_FALSE(P::WorldSaveService::validate_save(save));
        EXPECT_FALSE(
            P::InspectSavedWorld(root.path(), session.GetMetadata().worldId).error.empty());
        Luminumbra::world::WorldClock clock;
        bool required = false;
        EXPECT_FALSE(P::WorldSaveService::read_clock_metadata(save, clock, required));
        std::ofstream(save / "world_info.json", std::ios::binary) << valid_bytes;
    }
}

namespace {
void CheckEmptyEnergyResumeAlignsFirstDepositWithoutReplayingHistory() {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession control, loaded, explicit_load;
    for (auto* session : {&control, &loaded, &explicit_load}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetActiveRegionsEnabled(true);
        session->SetAetherStateEnabled(true);
    }
    ASSERT_TRUE(control.CreateWorld("Empty energy", "1337", "default"));
    const double dt = control.GetSimulationClock().fixed_dt();
    for (int i = 0; i < 317; ++i) {
        ASSERT_EQ(control.TickSimulation(dt), 1u);
    }
    ASSERT_TRUE(control.SaveWorldState());
    const auto save = control.GetWorldSaveDir();
    ASSERT_FALSE(fs::exists(save / "aether_state.efs"));
    ASSERT_TRUE(loaded.LoadWorld(control.GetMetadata().worldId));
    ASSERT_TRUE(explicit_load.CreateTransientWorld("Explicit", "1337", "default"));
    // An absent record must also clear a reused loader's pages and pending input.
    auto* previous = explicit_load.GetEnergyFieldState();
    previous->QueueDeposit(1, 0, 0, 0, 40000);
    ASSERT_EQ(explicit_load.TickSimulation(dt), 1u);
    previous->QueueDeposit(2, 0, 0, 0, 1000);
    ASSERT_TRUE(explicit_load.LoadWorldStateFrom(save));
    const auto empty_ages = control.GetActiveRegionLedger().energy_ages();
    for (auto* session : {&control, &loaded, &explicit_load}) {
        auto* energy = session->GetEnergyFieldState();
        ASSERT_EQ(energy->page_count(), 0u);
        EXPECT_EQ(energy->next_fire_tick(), 320u);
        EXPECT_EQ(session->GetActiveRegionLedger().energy_ages(), empty_ages);
        const auto fires = energy->fires_completed();
        energy->QueueDeposit(1, 0, 0, 0, 60000);
        ASSERT_EQ(session->TickSimulation(dt), 1u);
        EXPECT_EQ(energy->at_cell(0, 0), 60000u);
        EXPECT_EQ(energy->fires_completed(), fires);
    }
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(control));
        EXPECT_EQ(SessionWorldHash(explicit_load), SessionWorldHash(control));
        for (auto* session : {&control, &loaded, &explicit_load}) {
            ASSERT_EQ(session->TickSimulation(dt), 1u);
        }
    }
}

void CheckEmptyLegacyEnergySnapshotPreservesCadence(bool aligned) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession legacy, control, loaded, explicit_load;
    for (auto* session : {&legacy, &control, &loaded, &explicit_load}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetAetherStateEnabled(true);
        session->SetActiveRegionsEnabled(session != &legacy);
    }
    ASSERT_TRUE(legacy.CreateWorld("Legacy energy", "1337", "default"));
    const double dt = legacy.GetSimulationClock().fixed_dt();
    const int legacy_ticks = aligned ? 0 : 5;
    for (int i = 0; i < legacy_ticks; ++i) {
        ASSERT_EQ(legacy.TickSimulation(dt), 1u);
    }
    // The aligned case fires its cadence step at once, so the deposit decays away before the save
    // and the snapshot is an empty record that still has to keep its aligned schedule.
    legacy.GetEnergyFieldState()->QueueDeposit(1, 0, 0, 0, aligned ? 2 : 1);
    legacy.GetEnergyFieldState()->Tick(static_cast<std::uint64_t>(legacy_ticks));
    ASSERT_TRUE(legacy.SaveWorldState());
    const auto save = legacy.GetWorldSaveDir();
    ASSERT_FALSE(nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"))
                     .contains("simulationTick"));
    ASSERT_TRUE(control.LoadWorld(legacy.GetMetadata().worldId));
    ASSERT_EQ(control.GetSimulationTickCount(), 0u);
    ASSERT_EQ(control.GetEnergyFieldState()->total_raw(), aligned ? 0u : 1u);
    const Luminumbra::world::RegionKey owner{0, 0};
    const std::uint64_t initial_age = aligned ? 0u : 5u;
    if (aligned) {
        // An aligned empty snapshot carries no cadence to migrate: the regional schedule starts
        // clean.
        ASSERT_TRUE(control.GetActiveRegionLedger().energy_ages().empty());
    } else {
        ASSERT_EQ(control.GetActiveRegionLedger().energy_ages().at(owner), initial_age);
    }
    const int save_tick = aligned ? 8 : 7;
    const std::uint64_t next_fire = aligned ? 16u : 11u;
    for (int i = 0; i < save_tick; ++i) {
        ASSERT_EQ(control.TickSimulation(dt), 1u);
    }
    ASSERT_EQ(control.GetEnergyFieldState()->page_count(), 0u);
    if (!aligned) {
        ASSERT_EQ(control.GetActiveRegionLedger().energy_ages().at(owner),
                  initial_age + static_cast<std::uint64_t>(save_tick));
    }
    const auto explicit_save = root.path() / "empty-offset-snapshot";
    ASSERT_TRUE(control.SaveWorldStateTo(explicit_save));
    EXPECT_FALSE(fs::exists(explicit_save / "aether_state.efs"));
    ASSERT_TRUE(control.SaveWorldState());
    EXPECT_FALSE(fs::exists(save / "aether_state.efs"));
    ASSERT_TRUE(loaded.LoadWorld(control.GetMetadata().worldId));
    ASSERT_TRUE(explicit_load.CreateTransientWorld("Explicit", "1337", "default"));
    ASSERT_TRUE(explicit_load.LoadWorldStateFrom(explicit_save));
    for (auto* session : {&control, &loaded, &explicit_load}) {
        auto* energy = session->GetEnergyFieldState();
        ASSERT_EQ(energy->page_count(), 0u);
        if (!aligned) {
            EXPECT_EQ(session->GetActiveRegionLedger().energy_ages().at(owner),
                      initial_age + static_cast<std::uint64_t>(save_tick));
        }
        ASSERT_TRUE(energy->QueueRegionalDeposit(1, 0, 0, 0, 60000));
    }
    for (int tick = save_tick + 1; tick <= save_tick + 18; ++tick) {
        SCOPED_TRACE(tick);
        for (auto* session : {&control, &loaded, &explicit_load}) {
            ASSERT_EQ(session->TickSimulation(dt), 1u);
            if (static_cast<std::uint64_t>(tick) < next_fire) {
                EXPECT_EQ(session->GetEnergyFieldState()->at_cell(0, 0), 60000u);
            } else {
                EXPECT_LT(session->GetEnergyFieldState()->at_cell(0, 0), 60000u);
            }
        }
        for (auto* session : {&loaded, &explicit_load}) {
            EXPECT_EQ(session->GetEnergyFieldState()->SerializeRegionalRecord(),
                      control.GetEnergyFieldState()->SerializeRegionalRecord());
            EXPECT_EQ(session->GetActiveRegionLedger().energy_ages(),
                      control.GetActiveRegionLedger().energy_ages());
        }
        EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(control));
        EXPECT_EQ(SessionWorldHash(explicit_load), SessionWorldHash(control));
    }
}

void ExpectEmptyEnergySaveBytes(GameSession& live,
                                const fs::path& save,
                                const fs::path& explicit_save,
                                int channels) {
    if (channels == 2) {
        EXPECT_FALSE(fs::exists(save / "aether_state.efs"));
        EXPECT_FALSE(fs::exists(explicit_save / "aether_state.efs"));
        auto before = SavedPresetTestDiskBytes(save);
        ASSERT_TRUE(live.SaveWorldState());
        auto after = SavedPresetTestDiskBytes(save);
        // Only the snapshot generation changes on an otherwise identical save.
        before.erase("chunks/region/snapshot.json");
        after.erase("chunks/region/snapshot.json");
        EXPECT_EQ(after, before);
    } else {
        const auto expected = "EFS2 " + std::to_string(channels) + " 0\n";
        EXPECT_EQ(ReadClockTestTextRecord(save / "aether_state.efs"), expected);
        EXPECT_EQ(ReadClockTestTextRecord(explicit_save / "aether_state.efs"), expected);
    }
}

void ExpectReopenedEnergyTracksLive(GameSession& live,
                                    GameSession& reopened,
                                    GameSession& explicit_load,
                                    int channels) {
    const double dt = live.GetSimulationClock().fixed_dt();
    for (int tick = 1; tick <= 17; ++tick) {
        SCOPED_TRACE(tick);
        for (auto* session : {&live, &reopened, &explicit_load}) {
            ASSERT_EQ(session->TickSimulation(dt), 1u);
            ASSERT_FALSE(session->GetRegionSchedulerFault());
            EXPECT_EQ(session->GetEnergyFieldState()->channels(), channels);
            if (tick < 3) {
                EXPECT_EQ(session->GetEnergyFieldState()->at_cell(0, 0), 60000u);
            } else {
                EXPECT_LT(session->GetEnergyFieldState()->at_cell(0, 0), 60000u);
            }
        }
        for (auto* session : {&reopened, &explicit_load}) {
            EXPECT_EQ(session->GetEnergyFieldState()->CanonicalRegionalBytes(),
                      live.GetEnergyFieldState()->CanonicalRegionalBytes());
            EXPECT_EQ(session->GetEnergyFieldState()->SerializeRegionalRecord(),
                      live.GetEnergyFieldState()->SerializeRegionalRecord());
            EXPECT_EQ(session->GetActiveRegionLedger().energy_ages(),
                      live.GetActiveRegionLedger().energy_ages());
            EXPECT_EQ(session->ComputeAetherStateSubHash(), live.ComputeAetherStateSubHash());
            EXPECT_EQ(SessionWorldHash(*session), SessionWorldHash(live));
        }
    }
}

void ExpectLastChannelDepositMatches(GameSession& live,
                                     GameSession& reopened,
                                     GameSession& explicit_load,
                                     int channels) {
    const double dt = live.GetSimulationClock().fixed_dt();
    for (auto* session : {&live, &reopened, &explicit_load}) {
        ASSERT_TRUE(
            session->GetEnergyFieldState()->QueueRegionalDeposit(2, 0, 0, channels - 1, 12345));
        ASSERT_EQ(session->TickSimulation(dt), 1u);
        EXPECT_EQ(session->GetEnergyFieldState()->at_cell(0, 0, channels - 1), 12345u);
    }
    EXPECT_EQ(SessionWorldHash(reopened), SessionWorldHash(live));
    EXPECT_EQ(SessionWorldHash(explicit_load), SessionWorldHash(live));
}

void CheckEmptyLegacyEnergyChannelsContinueAfterReopen(int channels) {
    SCOPED_TRACE(channels);
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession legacy, live, reopened, explicit_load;
    for (auto* session : {&legacy, &live, &reopened, &explicit_load}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetAetherStateEnabled(true);
        session->SetActiveRegionsEnabled(session != &legacy);
    }
    ASSERT_TRUE(legacy.CreateWorld("Empty energy channels", "1337", "default"));
    ASSERT_TRUE(legacy.GetEnergyFieldState()->DeserializeRecord(
        "EFS1 " + std::to_string(channels) + " 3\n", 0));
    ASSERT_TRUE(legacy.SaveWorldState());
    ASSERT_TRUE(live.LoadWorld(legacy.GetMetadata().worldId)) << live.GetWorldOpenError();
    ASSERT_EQ(live.GetEnergyFieldState()->channels(), channels);
    ASSERT_EQ(live.GetEnergyFieldState()->page_count(), 0u);
    ASSERT_EQ(live.GetActiveRegionLedger().energy_ages().at({0, 0}), 5u);
    const auto canonical = live.GetEnergyFieldState()->CanonicalRegionalBytes();
    const auto hash = SessionWorldHash(live);
    ASSERT_TRUE(live.SaveWorldState());
    const auto save = live.GetWorldSaveDir();
    const auto explicit_save = root.path() / "empty-channel-snapshot";
    ASSERT_TRUE(live.SaveWorldStateTo(explicit_save));
    EXPECT_EQ(live.GetEnergyFieldState()->CanonicalRegionalBytes(), canonical);
    EXPECT_EQ(SessionWorldHash(live), hash);
    ASSERT_NO_FATAL_FAILURE(ExpectEmptyEnergySaveBytes(live, save, explicit_save, channels));
    ASSERT_TRUE(reopened.LoadWorld(live.GetMetadata().worldId)) << reopened.GetWorldOpenError();
    ASSERT_TRUE(explicit_load.CreateTransientWorld("Explicit channels", "1337", "default"));
    ASSERT_TRUE(explicit_load.GetEnergyFieldState()->DeserializeRecord("EFS1 4 3\n", 0));
    ASSERT_TRUE(explicit_load.LoadWorldStateFrom(explicit_save));
    for (auto* session : {&live, &reopened, &explicit_load}) {
        ASSERT_EQ(session->GetEnergyFieldState()->channels(), channels);
        EXPECT_EQ(session->GetEnergyFieldState()->page_count(), 0u);
        EXPECT_EQ(session->GetEnergyFieldState()->CanonicalRegionalBytes(), canonical);
        EXPECT_EQ(SessionWorldHash(*session), hash);
        ASSERT_TRUE(session->GetEnergyFieldState()->QueueRegionalDeposit(1, 0, 0, 0, 60000));
    }
    ASSERT_NO_FATAL_FAILURE(
        ExpectReopenedEnergyTracksLive(live, reopened, explicit_load, channels));
    if (channels > 2) {
        ASSERT_NO_FATAL_FAILURE(
            ExpectLastChannelDepositMatches(live, reopened, explicit_load, channels));
    }
    // Once populated, every channel count uses the existing nonempty record format.
    ASSERT_TRUE(live.SaveWorldState());
    const auto saved = ReadClockTestTextRecord(save / "aether_state.efs");
    EXPECT_TRUE(saved.starts_with("EFS1 " + std::to_string(channels) + " 0\n"));
    EXPECT_EQ(saved, live.GetEnergyFieldState()->SerializeRegionalRecord());
}
} // namespace

TEST(GameSessionHeadlessWorldTest, EmptyLegacyEnergyChannelsContinueAfterReopen) {
    for (const int channels : {1, 2, 5, 16}) {
        ASSERT_NO_FATAL_FAILURE(CheckEmptyLegacyEnergyChannelsContinueAfterReopen(channels));
    }
}

TEST(GameSessionHeadlessWorldTest, EmptyEnergyResumeAlignsFirstDepositWithoutReplayingHistory) {
    ASSERT_NO_FATAL_FAILURE(CheckEmptyEnergyResumeAlignsFirstDepositWithoutReplayingHistory());
}

TEST(GameSessionHeadlessWorldTest, EmptyLegacyEnergySnapshotPreservesNonAlignedCadence) {
    ASSERT_NO_FATAL_FAILURE(CheckEmptyLegacyEnergySnapshotPreservesCadence(false));
}

TEST(GameSessionHeadlessWorldTest, EmptyAlignedLegacyEnergyRetainsModeAcrossReopen) {
    ASSERT_NO_FATAL_FAILURE(CheckEmptyLegacyEnergySnapshotPreservesCadence(true));
}

namespace {
void CheckEmptyAlignedEnergySaveRemovesStaleRecord() {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original, loaded;
    for (auto* session : {&original, &loaded}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetActiveRegionsEnabled(true);
        session->SetAetherStateEnabled(true);
    }
    ASSERT_TRUE(original.CreateWorld("Aligned energy", "1337", "default"));
    const double dt = original.GetSimulationClock().fixed_dt();
    original.GetEnergyFieldState()->QueueDeposit(1, 0, 0, 0, 1);
    ASSERT_EQ(original.TickSimulation(dt), 1u);
    ASSERT_TRUE(original.SaveWorldState());
    ASSERT_TRUE(fs::exists(original.GetWorldSaveDir() / "aether_state.efs"));
    for (int i = 1; i < 8; ++i) {
        ASSERT_EQ(original.TickSimulation(dt), 1u);
    }
    ASSERT_EQ(original.GetEnergyFieldState()->page_count(), 0u);
    ASSERT_TRUE(original.SaveWorldState());
    EXPECT_FALSE(fs::exists(original.GetWorldSaveDir() / "aether_state.efs"));
    ASSERT_TRUE(loaded.LoadWorld(original.GetMetadata().worldId));
    EXPECT_EQ(loaded.GetEnergyFieldState()->total_raw(), 0u);
    EXPECT_EQ(loaded.GetEnergyFieldState()->next_fire_tick(), 16u);
    EXPECT_EQ(loaded.GetActiveRegionLedger().energy_ages(),
              original.GetActiveRegionLedger().energy_ages());
    EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(original));
}

} // namespace

TEST(GameSessionHeadlessWorldTest, EmptyAlignedEnergySaveRemovesStaleRecord) {
    ASSERT_NO_FATAL_FAILURE(CheckEmptyAlignedEnergySaveRemovesStaleRecord());
}

TEST(GameSessionHeadlessWorldTest, TransientClockSnapshotRefusesBeforeWritingAnyMember) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original, transient;
    for (auto* session : {&original, &transient}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetActiveRegionsEnabled(true);
        session->SetAetherStateEnabled(true);
    }
    ASSERT_TRUE(original.CreateWorld("Snapshot owner", "1337", "default"));
    ClockTestPlant(original.GetRegistry(), original.GetMetadata().spawnPoint);
    original.GetEnergyFieldState()->QueueDeposit(1, 0, 0, 0, 60000);
    ASSERT_EQ(original.TickSimulation(original.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_TRUE(original.SaveWorldState());
    const auto save = original.GetWorldSaveDir();
    ASSERT_TRUE(transient.CreateTransientWorld("Snapshot reader", "1337", "default"));
    ASSERT_TRUE(transient.LoadWorldStateFrom(save));
    // Make every member writer observable if it runs before the refusal.
    ClockTestPlant(transient.GetRegistry(), transient.GetMetadata().spawnPoint);
    transient.GetEnergyFieldState()->QueueDeposit(2, 0, 0, 0, 1000);
    ASSERT_EQ(transient.TickSimulation(transient.GetSimulationClock().fixed_dt()), 1u);
    const auto snapshot_bytes = [](const fs::path& path) {
        std::map<fs::path, std::string> bytes;
        for (const auto& entry : fs::recursive_directory_iterator(path))
            bytes.emplace(entry.path().lexically_relative(path),
                          entry.is_regular_file() ? ReadClockTestFile(entry.path()) : "");
        return bytes;
    };
    const auto before = snapshot_bytes(save);
    const auto missing = root.path() / "refused-transient-snapshot";
    for (const bool dirty : {false, true}) {
        SCOPED_TRACE(dirty);
        if (dirty) {
            auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
            chunk->set_state(Luminumbra::ChunkState::Idle);
            chunk->mark_voxel_data_dirty();
            ASSERT_TRUE(transient.GetWorldSystem()->adopt_streamed_chunk(chunk));
        }
        EXPECT_FALSE(transient.SaveWorldStateTo(save));
        EXPECT_EQ(snapshot_bytes(save), before);
        EXPECT_FALSE(transient.SaveWorldStateTo(missing));
        EXPECT_FALSE(fs::exists(missing));
        EXPECT_FALSE(transient.SaveWorld());
    }
}

TEST(GameSessionHeadlessWorldTest, EnergyOnlyExplicitSnapshotCreatesDestinationAndRoundTrips) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original, loaded;
    for (auto* session : {&original, &loaded}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetActiveRegionsEnabled(true);
        session->SetAetherStateEnabled(true);
    }
    ASSERT_TRUE(original.CreateWorld("Energy only", "1337", "default"));
    original.GetEnergyFieldState()->QueueDeposit(1, 0, 0, 0, 60000);
    const double dt = original.GetSimulationClock().fixed_dt();
    ASSERT_EQ(original.TickSimulation(dt), 1u);
    const auto save = root.path() / "energy-only";
    ASSERT_FALSE(fs::exists(save));
    Luminumbra::world::WorldStateSaveReport report;
    ASSERT_TRUE(original.SaveWorldStateTo(save, &report));
    EXPECT_EQ(report.chunks_dirty, 0u);
    EXPECT_TRUE(fs::exists(P::WorldSaveService::active_regions_path(save)));
    EXPECT_TRUE(fs::exists(save / "aether_state.efs"));
    ASSERT_TRUE(loaded.CreateTransientWorld("Energy reader", "1337", "default"));
    ASSERT_TRUE(loaded.LoadWorldStateFrom(save));
    EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(original));
    for (int i = 0; i < 10; ++i) {
        ASSERT_EQ(original.TickSimulation(dt), 1u);
        ASSERT_EQ(loaded.TickSimulation(dt), 1u);
        EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(original));
    }
}

TEST(GameSessionHeadlessWorldTest, ExplicitlyDisabledCallbacksKeepLegacySavesLoadsAndNestedTicks) {
    const HeadlessRoot root;
    {
        const auto path = root.path() / "worlds/atlas/presets/default.json";
        std::ifstream input(path);
        auto preset = nlohmann::json::parse(input);
        input.close();
        preset["generation_params"]["terrain"]["island_mask_enabled"] = false;
        std::ofstream output(path);
        output << preset.dump(2);
    }
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(false);
    session.SetAetherStateEnabled(false);
    session.SetWaterHighResEnabled(false);
    session.SetWeatherRainEnabled(false);
    session.SetWeatherEventsEnabled(false);
    session.SetFiniteHydrologyEnabled(false);

    ASSERT_TRUE(session.CreateWorld("Legacy callbacks", "1337", "default"));
    ASSERT_TRUE(session.LoadWorldStateFrom(root.path() / "unmarked-snapshot"));
    ASSERT_TRUE(session.SaveWorld());
    const auto save = session.GetWorldSaveDir();
    const auto before = ReadClockTestFile(save / "world_info.json");
    const double dt = session.GetSimulationClock().fixed_dt();
    bool called = false;
    session.GetSimulationEventBus().subscribe([&](const auto&) {
        called = true;
        EXPECT_TRUE(session.IsSimulationTickBoundary());
        EXPECT_TRUE(session.SaveWorld());
        EXPECT_TRUE(session.SaveWorldState());
        EXPECT_TRUE(session.SaveWorldStateTo(
            root.path() / "legacy-explicit", nullptr, /*full_snapshot=*/true));
        EXPECT_EQ(ReadClockTestFile(root.path() / "legacy-explicit/world_info.json"), before);
        EXPECT_TRUE(P::WorldSaveService::validate_save(root.path() / "legacy-explicit"));
        EXPECT_TRUE(session.LoadWorldState());
        EXPECT_TRUE(session.LoadWorldStateFrom(save));
        EXPECT_EQ(session.TickSimulation(dt), 1u);
        EXPECT_EQ(session.GetSimulationTickCount(), 2u);
        EXPECT_EQ(ReadClockTestFile(save / "world_info.json"), before);
        // Failed world opens historically reached catalog validation even in a
        // callback. A successful open clears the bus, so do not mutate its handlers.
        EXPECT_FALSE(session.LoadWorld("missing-world"));
        EXPECT_FALSE(session.GetWorldOpenError().empty());
    });
    session.GetSimulationEventBus().publish(1, "legacy-callback", "");
    EXPECT_EQ(session.TickSimulation(dt), 1u);
    EXPECT_TRUE(called);
}

TEST(GameSessionHeadlessWorldTest, ClockSnapshotsRefuseInsideCatchUpBatch) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Boundary", "1337", "default"));
    const auto metadata = session.GetWorldSaveDir() / "world_info.json";
    const auto before = ReadClockTestFile(metadata);
    int callbacks = 0;
    session.GetSimulationEventBus().subscribe([&](const auto& event) {
        ++callbacks;
        EXPECT_EQ(session.GetSimulationTickCount(), event.tick);
        EXPECT_FALSE(session.IsSimulationTickBoundary());
        EXPECT_FALSE(session.SaveWorld());
        EXPECT_FALSE(session.SaveWorldState());
        EXPECT_FALSE(session.SaveWorldStateTo(root.path() / "reentrant-save"));
        EXPECT_FALSE(session.LoadWorldStateFrom(session.GetWorldSaveDir()));
        EXPECT_FALSE(session.LoadWorld(session.GetMetadata().worldId));
        EXPECT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 0u);
        EXPECT_EQ(ReadClockTestFile(metadata), before);
    });
    session.GetSimulationEventBus().publish(1, "save", "");
    session.GetSimulationEventBus().publish(2, "save", "");
    EXPECT_EQ(session.TickSimulation(2 * session.GetSimulationClock().fixed_dt()), 2u);
    EXPECT_EQ(callbacks, 2);
    EXPECT_TRUE(session.IsSimulationTickBoundary());
    ASSERT_TRUE(session.SaveWorldState());
    EXPECT_EQ(nlohmann::json::parse(ReadClockTestFile(metadata)).at("simulationTick"), 2u);
}

TEST(GameSessionHeadlessWorldTest, ExplicitlyDisabledKeepsLegacyMetadataBytesAndTickZeroLoad) {
    const HeadlessRoot root;
    {
        const auto path = root.path() / "worlds/atlas/presets/default.json";
        std::ifstream input(path);
        auto preset = nlohmann::json::parse(input);
        input.close();
        preset["generation_params"]["terrain"]["island_mask_enabled"] = false;
        std::ofstream output(path);
        output << preset.dump(2);
    }
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, loaded;
    for (auto* s : {&session, &loaded}) {
        s->SetRootPath(root.root_string());
        s->SetJobSystem(&jobs);
        s->SetActiveRegionsEnabled(false);
        s->SetAetherStateEnabled(false);
        s->SetWaterHighResEnabled(false);
        s->SetWeatherRainEnabled(false);
        s->SetWeatherEventsEnabled(false);
        s->SetFiniteHydrologyEnabled(false);
    }
    ASSERT_TRUE(session.CreateWorld("Legacy clock", "1337", "default"));
    const auto path = session.GetWorldSaveDir() / "world_info.json";
    const auto& m = session.GetMetadata();
    // Clock keys remain absent; the preset marker and identity apply to every new world.
    const nlohmann::json legacy = {
        {"container_version", 2},
        {"presetFormat", 1},
        {"presetIdentity",
         P::StableChecksum(
             nlohmann::json::parse(ReadClockTestFile(session.GetWorldSaveDir() / "preset.json"))
                 .dump())},
        {"name", m.name},
        {"seed", m.seed},
        {"worldType", m.worldType},
        {"creationTime", m.creationTime},
        {"spawnPoint", {{"x", m.spawnPoint.x}, {"y", m.spawnPoint.y}, {"z", m.spawnPoint.z}}},
        {"waterSimCursor", std::size_t{0}}};
    const auto before = legacy.dump(4) + "\n";
    std::ofstream(path, std::ios::binary | std::ios::trunc) << before;
    ASSERT_TRUE(session.LoadWorld(m.worldId));
    EXPECT_EQ(session.GetRenderCalendar(), Luminumbra::world::WorldCalendar::Legacy());
    for (int i = 0; i < 10; ++i)
        session.TickSimulation(session.GetSimulationClock().fixed_dt());
    EXPECT_EQ(session.FoldClockIntoEcologyHash("legacy-ecology"), "legacy-ecology");
    ASSERT_TRUE(session.SaveWorldState());
    ASSERT_TRUE(session.SaveWorld());
    EXPECT_EQ(ReadClockTestFile(path), before);
    ASSERT_TRUE(loaded.LoadWorld(m.worldId));
    EXPECT_EQ(loaded.GetSimulationTickCount(), 0u);
    EXPECT_EQ(ReadClockTestFile(path), before);
}

TEST(GameSessionHeadlessWorldTest, RenderCalendarMarkerLifecycleAndSnapshotReplacement) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, loaded;
    for (auto* s : {&session, &loaded}) {
        s->SetRootPath(root.root_string());
        s->SetJobSystem(&jobs);
        s->SetActiveRegionsEnabled(false);
    }
    ASSERT_TRUE(session.CreateWorld("Presentation calendar", "1337", "default"));
    const auto id = session.GetMetadata().worldId;
    const auto save = session.GetWorldSaveDir();
    const auto before = ReadClockTestFile(save / "world_info.json");
    const auto metadata = nlohmann::json::parse(before);
    EXPECT_FALSE(metadata.contains("simulationTick"));
    EXPECT_FALSE(metadata.contains("calendar"));
    ASSERT_TRUE(metadata.contains("renderCalendar"));
    EXPECT_EQ(metadata.at("renderCalendar").at("version"), 2);
    EXPECT_EQ(session.GetRenderCalendar(), Luminumbra::world::WorldCalendar{});
    const auto inspected = P::InspectSavedWorld(root.path(), id);
    ASSERT_TRUE(inspected.error.empty()) << inspected.error;
    EXPECT_FALSE(inspected.requires_active_regions);
    ASSERT_TRUE(inspected.render_calendar.has_value());
    EXPECT_EQ(inspected.render_calendar,
              std::optional<Luminumbra::world::WorldCalendar>{Luminumbra::world::WorldCalendar{}});
    ASSERT_TRUE(loaded.LoadWorld(id));
    EXPECT_EQ(loaded.GetRenderCalendar(), session.GetRenderCalendar());
    EXPECT_EQ(ReadClockTestFile(save / "world_info.json"), before);
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    EXPECT_EQ(session.FoldClockIntoEcologyHash("ecology"), "ecology");
    ASSERT_TRUE(session.SaveWorld());
    EXPECT_EQ(ReadClockTestFile(save / "world_info.json"), before);
    const auto snapshot = root.path() / "marked-snapshot";
    ASSERT_TRUE(session.SaveWorldStateTo(snapshot));
    EXPECT_EQ(
        nlohmann::json::parse(ReadClockTestFile(snapshot / "world_info.json")).at("renderCalendar"),
        metadata.at("renderCalendar"));
    ASSERT_TRUE(loaded.LoadWorldStateFrom(snapshot));
    EXPECT_EQ(loaded.GetRenderCalendar(), Luminumbra::world::WorldCalendar{});
    const Luminumbra::world::WorldCalendar custom{101, 3, 3, 5, 9};
    auto custom_metadata = metadata;
    custom_metadata["renderCalendar"] =
        Luminumbra::world::WorldClock(0, custom).calendar_metadata();
    ASSERT_TRUE(P::WorldSaveService::save_metadata(custom_metadata.dump(), snapshot));
    ASSERT_TRUE(loaded.LoadWorldStateFrom(snapshot));
    EXPECT_EQ(loaded.GetRenderCalendar(), custom);
    ASSERT_TRUE(loaded.SaveWorld());
    EXPECT_EQ(
        nlohmann::json::parse(ReadClockTestFile(save / "world_info.json")).at("renderCalendar"),
        custom_metadata.at("renderCalendar"));
    ASSERT_TRUE(loaded.LoadWorldStateFrom(root.path() / "absent-metadata"));
    EXPECT_EQ(loaded.GetRenderCalendar(), Luminumbra::world::WorldCalendar::Legacy());
    ASSERT_TRUE(loaded.SaveWorld());
    EXPECT_FALSE(nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"))
                     .contains("renderCalendar"));
    ASSERT_TRUE(loaded.LoadWorldStateFrom(snapshot));
    EXPECT_EQ(loaded.GetRenderCalendar(), custom);
    ASSERT_TRUE(loaded.CreateWorld("Session reused", "1337", "default"));
    EXPECT_EQ(loaded.GetRenderCalendar(), Luminumbra::world::WorldCalendar{});
    // A transient presentation preview can also export an explicit snapshot.
    ASSERT_TRUE(loaded.CreateTransientWorld("Preview", "1337", "default"));
    ASSERT_TRUE(loaded.SaveWorldStateTo(root.path() / "preview-snapshot"));
}

TEST(GameSessionHeadlessWorldTest, UnmarkedSnapshotOverwriteRemovesRenderCalendarOnEverySavePath) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession marked;
    marked.SetRootPath(root.root_string());
    marked.SetJobSystem(&jobs);
    marked.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(marked.CreateWorld("Marked calendar", "1337", "default"));

    for (const bool transient : {false, true}) {
        SCOPED_TRACE(transient);
        GameSession legacy, loaded;
        for (auto* session : {&legacy, &loaded}) {
            session->SetRootPath(root.root_string());
            session->SetJobSystem(&jobs);
            session->SetActiveRegionsEnabled(false);
        }
        if (transient) {
            ASSERT_TRUE(legacy.CreateTransientWorld("Legacy preview", "1337", "default"));
        } else {
            ASSERT_TRUE(legacy.CreateWorld("Legacy calendar", "1337", "default"));
        }
        ASSERT_TRUE(legacy.LoadWorldStateFrom(root.path() / "absent-calendar"));
        ASSERT_EQ(legacy.GetRenderCalendar(), Luminumbra::world::WorldCalendar::Legacy());
        ASSERT_TRUE(loaded.CreateTransientWorld("Calendar reader", "1337", "default"));
        auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
        chunk->set_state(Luminumbra::ChunkState::Idle);

        for (int save_path = 0; save_path < 3; ++save_path) {
            SCOPED_TRACE(save_path);
            const auto snapshot = root.path() / "calendar-overwrite";
            fs::remove_all(snapshot);
            ASSERT_TRUE(marked.SaveWorldStateTo(snapshot));
            ASSERT_TRUE(nlohmann::json::parse(ReadClockTestFile(snapshot / "world_info.json"))
                            .contains("renderCalendar"));
            if (save_path != 0) {
                chunk->mark_voxel_data_dirty();
                if (save_path == 1) {
                    ASSERT_TRUE(legacy.GetWorldSystem()->adopt_streamed_chunk(chunk));
                }
                if (save_path == 2) {
                    Luminumbra::WorldStreamingState state;
                    state.insert_chunk(chunk);
                    ASSERT_TRUE(P::WorldSaveService{}.save_world(state, snapshot));
                }
            }
            EXPECT_EQ(P::WorldSaveService::has_world_save(snapshot), save_path == 2);
            Luminumbra::world::WorldStateSaveReport report;
            ASSERT_TRUE(legacy.SaveWorldStateTo(snapshot, &report));
            EXPECT_EQ(report.chunks_dirty, save_path == 0 ? 0u : 1u);
            EXPECT_FALSE(nlohmann::json::parse(ReadClockTestFile(snapshot / "world_info.json"))
                             .contains("renderCalendar"));
            ASSERT_TRUE(loaded.LoadWorldStateFrom(snapshot));
            EXPECT_EQ(loaded.GetRenderCalendar(), Luminumbra::world::WorldCalendar::Legacy());
        }
    }
}

TEST(GameSessionHeadlessWorldTest, EnergyWriteFailurePreservesCalendarMetadataOnEverySavePath) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    for (const bool marked_source : {false, true}) {
        SCOPED_TRACE(marked_source);
        GameSession session;
        session.SetRootPath(root.root_string());
        session.SetJobSystem(&jobs);
        session.SetActiveRegionsEnabled(false);
        session.SetAetherStateEnabled(true);
        ASSERT_TRUE(session.CreateWorld(
            marked_source ? "Marked energy failure" : "Legacy energy failure", "1337", "default"));
        if (!marked_source) {
            ASSERT_TRUE(session.LoadWorldStateFrom(root.path() / "absent-calendar"));
        }
        auto* energy = session.GetEnergyFieldState();
        ASSERT_NE(energy, nullptr);
        energy->SetAnchorCell(0, 0);
        energy->QueueDeposit(1, 0, 0, 0, 60000);
        energy->Tick(0);
        ASSERT_GT(energy->page_count(), 0u);
        auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
        chunk->set_state(Luminumbra::ChunkState::Idle);
        for (int save_path = 0; save_path < 3; ++save_path) {
            SCOPED_TRACE(save_path);
            if (save_path == 1) {
                ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
            }
            for (const bool marked_destination : {false, true}) {
                SCOPED_TRACE(marked_destination);
                const auto snapshot = root.path() / "failed-energy-snapshot";
                fs::remove_all(snapshot);
                ASSERT_TRUE(fs::create_directories(snapshot));
                auto metadata = nlohmann::json::parse(
                    ReadClockTestFile(session.GetWorldSaveDir() / "world_info.json"));
                metadata.erase("renderCalendar");
                if (marked_destination) {
                    metadata["renderCalendar"] =
                        Luminumbra::world::WorldClock(0, {101, 3, 3, 5, 9}).calendar_metadata();
                }
                ASSERT_TRUE(P::WorldSaveService::save_metadata(metadata.dump(), snapshot));
                if (save_path == 2) {
                    Luminumbra::WorldStreamingState state;
                    state.insert_chunk(chunk);
                    ASSERT_TRUE(P::WorldSaveService{}.save_world(state, snapshot));
                }
                chunk->mark_voxel_data_dirty();
                ASSERT_EQ(P::WorldSaveService::has_world_save(snapshot), save_path == 2);
                ASSERT_TRUE(P::WorldSaveService::validate_save(snapshot));
                const auto before = ReadClockTestFile(snapshot / "world_info.json");
                // A directory at the record path denies opening a file on both platforms,
                // independently of privileges, symlink support or permission bits.
                ASSERT_TRUE(fs::create_directory(snapshot / "aether_state.efs"));
                ASSERT_TRUE(P::WorldSaveService::validate_save(snapshot));
                Luminumbra::world::WorldStateSaveReport report;
                EXPECT_FALSE(session.SaveWorldStateTo(snapshot, &report));
                EXPECT_FALSE(report.saved);
                EXPECT_EQ(ReadClockTestFile(snapshot / "world_info.json"), before);
                EXPECT_TRUE(fs::is_directory(snapshot / "aether_state.efs"));
            }
        }
    }
}

TEST(GameSessionHeadlessWorldTest,
     CalendarIdentityRejectsDifferentResolvedCalendarsBeforeAdmission) {
    using Luminumbra::world::WorldCalendar;
    using Luminumbra::world::WorldClock;
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original;
    original.SetRootPath(root.root_string());
    original.SetJobSystem(&jobs);
    original.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(original.CreateWorld("Calendar identity", "1337", "default"));
    const auto snapshot = root.path() / "identity-calendar";
    ASSERT_TRUE(original.SaveWorldStateTo(snapshot));
    const auto metadata = nlohmann::json::parse(ReadClockTestFile(snapshot / "world_info.json"));
    for (const bool active_regions : {false, true}) {
        SCOPED_TRACE(active_regions);
        GameSession first, second;
        for (auto* session : {&first, &second}) {
            session->SetRootPath(root.root_string());
            session->SetJobSystem(&jobs);
            session->SetActiveRegionsEnabled(active_regions);
            ASSERT_TRUE(session->CreateTransientWorld("Admission", "1337", "default"));
        }
        for (const auto calendar : {WorldCalendar{},
                                    WorldCalendar{162001, 8, 2, 3, 8},
                                    WorldCalendar{162000, 9, 2, 3, 8},
                                    WorldCalendar{162000, 8, 1, 3, 8},
                                    WorldCalendar{162000, 8, 2, 5, 8},
                                    WorldCalendar{162000, 8, 2, 3, 9},
                                    WorldCalendar::Legacy()}) {
            SCOPED_TRACE(WorldClock(0, calendar).calendar_metadata().dump());
            ASSERT_TRUE(P::WorldSaveService::save_metadata(metadata.dump(), snapshot));
            ASSERT_TRUE(first.LoadWorldStateFrom(snapshot));
            auto changed_metadata = metadata;
            changed_metadata["renderCalendar"] = WorldClock(0, calendar).calendar_metadata();
            ASSERT_TRUE(P::WorldSaveService::save_metadata(changed_metadata.dump(), snapshot));
            ASSERT_TRUE(second.LoadWorldStateFrom(snapshot));
            const bool equal = calendar == WorldCalendar{};
            EXPECT_EQ(first.SystemConfigIdentity() == second.SystemConfigIdentity(), equal);
            Luminumbra::Net::HelloMsg local;
            local.seed = 1337;
            local.preset = "default";
            local.preset_content_hash = first.PresetContentHash();
            local.system_config_identity = first.SystemConfigIdentity();
            auto peer = local;
            peer.system_config_identity = second.SystemConfigIdentity();
            Luminumbra::Net::HelloMsg decoded;
            ASSERT_TRUE(Luminumbra::Net::DecodeHello(Luminumbra::Net::EncodeHello(peer), decoded));
            EXPECT_EQ(Luminumbra::Net::HelloMatches(local, decoded), equal);
            ASSERT_EQ(first.TickSimulation(first.GetSimulationClock().fixed_dt()), 1u);
            EXPECT_EQ(first.SystemConfigIdentity(), local.system_config_identity);
        }
        // The resolved legacy calendar is identical with or without a v2 marker.
        const auto legacy_identity = second.SystemConfigIdentity();
        auto unmarked_metadata = metadata;
        unmarked_metadata.erase("renderCalendar");
        ASSERT_TRUE(P::WorldSaveService::save_metadata(unmarked_metadata.dump(), snapshot));
        ASSERT_TRUE(second.LoadWorldStateFrom(snapshot));
        EXPECT_EQ(second.GetRenderCalendar(), WorldCalendar::Legacy());
        EXPECT_EQ(second.SystemConfigIdentity(), legacy_identity);
    }
}

TEST(GameSessionHeadlessWorldTest, RenderCalendarActivatesOnNormalLoadAndNextSave) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original, loaded;
    for (auto* session : {&original, &loaded}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
    }
    original.SetActiveRegionsEnabled(false);
    loaded.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(original.CreateWorld("Calendar activation", "1337", "default"));
    for (int i = 0; i < 3; ++i)
        ASSERT_EQ(original.TickSimulation(original.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_TRUE(original.SaveWorldState());
    const auto save = original.GetWorldSaveDir();
    const Luminumbra::world::WorldCalendar custom{101, 3, 3, 5, 9};
    const auto recorded = Luminumbra::world::WorldClock(0, custom).calendar_metadata();
    auto metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    ASSERT_FALSE(metadata.contains("simulationTick"));
    ASSERT_FALSE(metadata.contains("calendar"));
    metadata["renderCalendar"] = recorded;
    ASSERT_TRUE(P::WorldSaveService::save_metadata(metadata.dump(), save));
    const auto before = ReadClockTestDirectory(root.path());

    ASSERT_TRUE(loaded.LoadWorld(original.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_EQ(loaded.GetWorldClock().calendar(), custom);
    EXPECT_EQ(loaded.GetRenderCalendar(), custom);
    EXPECT_EQ(loaded.GetWorldClock().tick(), 0u);
    EXPECT_EQ(loaded.GetSimulationClock().tick_count(), 0u);
    EXPECT_EQ(loaded.GetActiveRegionLedger().tick(), 0u);
    EXPECT_EQ(ReadClockTestDirectory(root.path()), before);

    ASSERT_TRUE(loaded.SaveWorldState());
    const auto saved = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    EXPECT_EQ(saved.at("simulationTick"), 0u);
    EXPECT_EQ(saved.at("calendar"), recorded);
    EXPECT_FALSE(saved.contains("renderCalendar"));
    ASSERT_TRUE(loaded.LoadWorld(original.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_EQ(loaded.GetWorldClock().calendar(), custom);
    EXPECT_EQ(loaded.GetWorldClock().tick(), 0u);
}

TEST(GameSessionHeadlessWorldTest, RenderCalendarActivatesOnExplicitSnapshotLoadAndNextSave) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original, loaded;
    for (auto* session : {&original, &loaded}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
    }
    original.SetActiveRegionsEnabled(false);
    loaded.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(original.CreateWorld("Presentation snapshot", "1337", "default"));
    const auto snapshot = root.path() / "activation-snapshot";
    ASSERT_TRUE(original.SaveWorldStateTo(snapshot));
    const Luminumbra::world::WorldCalendar custom{101, 3, 3, 5, 9};
    const auto recorded = Luminumbra::world::WorldClock(0, custom).calendar_metadata();
    auto metadata = nlohmann::json::parse(ReadClockTestFile(snapshot / "world_info.json"));
    ASSERT_FALSE(metadata.contains("simulationTick"));
    ASSERT_FALSE(metadata.contains("calendar"));
    metadata["renderCalendar"] = recorded;
    ASSERT_TRUE(P::WorldSaveService::save_metadata(metadata.dump(), snapshot));
    ASSERT_TRUE(loaded.CreateWorld("Snapshot activation", "1337", "default"));
    for (int i = 0; i < 3; ++i)
        ASSERT_EQ(loaded.TickSimulation(loaded.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_NE(loaded.GetWorldClock().calendar(), custom);
    ASSERT_EQ(loaded.GetWorldClock().tick(), 3u);
    const auto before = ReadClockTestDirectory(root.path());

    ASSERT_TRUE(loaded.LoadWorldStateFrom(snapshot)) << loaded.GetWorldOpenError();
    EXPECT_EQ(loaded.GetWorldClock().calendar(), custom);
    EXPECT_EQ(loaded.GetRenderCalendar(), custom);
    EXPECT_EQ(loaded.GetWorldClock().tick(), 0u);
    EXPECT_EQ(loaded.GetSimulationClock().tick_count(), 0u);
    EXPECT_EQ(loaded.GetActiveRegionLedger().tick(), 0u);
    EXPECT_EQ(ReadClockTestDirectory(root.path()), before);

    ASSERT_TRUE(loaded.SaveWorldStateTo(snapshot));
    const auto saved = nlohmann::json::parse(ReadClockTestFile(snapshot / "world_info.json"));
    EXPECT_EQ(saved.at("simulationTick"), 0u);
    EXPECT_EQ(saved.at("calendar"), recorded);
    EXPECT_FALSE(saved.contains("renderCalendar"));
    ASSERT_TRUE(loaded.LoadWorldStateFrom(snapshot)) << loaded.GetWorldOpenError();
    EXPECT_EQ(loaded.GetWorldClock().calendar(), custom);
    EXPECT_EQ(loaded.GetWorldClock().tick(), 0u);
}

TEST(GameSessionHeadlessWorldTest, DefaultCalendarContinuesAcrossSaveAndExplicitSnapshot) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original, loaded;
    for (auto* s : {&original, &loaded}) {
        s->SetRootPath(root.root_string());
        s->SetJobSystem(&jobs);
        s->SetActiveRegionsEnabled(true);
    }
    ASSERT_TRUE(original.CreateWorld("Default calendar", "1337", "default"));
    ClockTestPlant(original.GetRegistry(), original.GetMetadata().spawnPoint);
    const auto snapshot = root.path() / "default-calendar-snapshot";
    for (int i = 0; i < 31; ++i)
        ASSERT_EQ(original.TickSimulation(original.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_TRUE(original.SaveWorldStateTo(snapshot));
    ASSERT_TRUE(loaded.CreateTransientWorld("Continuation", "1337", "default"));
    ASSERT_TRUE(loaded.LoadWorldStateFrom(snapshot));
    EXPECT_EQ(loaded.GetWorldClock().calendar(), Luminumbra::world::WorldCalendar{});
    EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(original));
    for (int i = 0; i < 31; ++i) {
        ASSERT_EQ(original.TickSimulation(original.GetSimulationClock().fixed_dt()), 1u);
        ASSERT_EQ(loaded.TickSimulation(loaded.GetSimulationClock().fixed_dt()), 1u);
    }
    EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(original));
}

TEST(GameSessionHeadlessWorldTest, EnabledLegacyClockPinsEmptyEcologyFold) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Legacy clock fold", "1337", "default"));
    const auto save = session.GetWorldSaveDir();
    auto metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    Luminumbra::world::WorldClock(12345, Luminumbra::world::WorldCalendar::Legacy())
        .write_metadata(metadata);
    ASSERT_TRUE(P::WorldSaveService::save_metadata(metadata.dump(), save));
    ASSERT_TRUE(session.LoadWorldStateFrom(save));
    ASSERT_EQ(session.GetWorldClock().tick(), 12345u);
    ASSERT_EQ(session.GetWorldClock().calendar(), Luminumbra::world::WorldCalendar::Legacy());
    ASSERT_TRUE(session.GetActiveRegionLedger().records().empty());
    ASSERT_TRUE(session.GetActiveRegionLedger().canonical_bytes().empty());
    const auto ecology = luminumbra::ai::ComputeEcologySubHash(session.GetRegistry());
    ASSERT_TRUE(ecology.empty());
    EXPECT_EQ(session.FoldClockIntoEcologyHash(ecology), "9b7d0612513877a2");
}

TEST(GameSessionHeadlessWorldTest, EnabledClockOverflowRefusesWithoutAdvancingEitherClock) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Clock limit", "1337", "default"));
    const auto save = session.GetWorldSaveDir();
    auto metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    const auto last = Luminumbra::world::WorldClock::kTickLimit - 1;
    Luminumbra::world::WorldClock(last).write_metadata(metadata);
    ASSERT_TRUE(P::WorldSaveService::save_metadata(metadata.dump(), save));
    ASSERT_TRUE(session.LoadWorldStateFrom(save));
    const auto before = SessionWorldHash(session);
    EXPECT_THROW(session.TickSimulation(session.GetSimulationClock().fixed_dt()),
                 std::overflow_error);
    EXPECT_EQ(session.GetSimulationTickCount(), last);
    EXPECT_EQ(session.GetSimulationClock().tick_count(), last);
    EXPECT_EQ(session.GetSimulationClock().accumulator(), 0.0);
    EXPECT_EQ(SessionWorldHash(session), before);
    EXPECT_TRUE(session.IsSimulationTickBoundary());
}

TEST(GameSessionHeadlessWorldTest, CalendarConsumersUsePinnedPhasesOnTheRealTickPath) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Phases", "1337", "default"));
    const auto id = session.GetMetadata().worldId;
    const auto save = session.GetWorldSaveDir();
    auto metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    struct Pin {
        std::uint64_t tick;
        float day, year, daylight, warmth;
        Luminumbra::world::WorldCalendar calendar{120, 4};
    };
    for (const auto pin : {Pin{20, .25f, 1.0f / 24.0f, .575f, 1.0f / 12.0f},
                           Pin{30, .3203125f, .0625f, .69453125f, .125f},
                           Pin{60, .5f, .125f, 1.0f, .25f},
                           Pin{90, .6796875f, .1875f, .69453125f, .375f},
                           Pin{100, .75f, 5.0f / 24.0f, .575f, 5.0f / 12.0f},
                           Pin{120, 0.0f, .25f, .15f, .5f},
                           Pin{240, 0.0f, .5f, .15f, 1.0f},
                           Pin{360, 0.0f, .75f, .15f, .5f},
                           Pin{480, 0.0f, 0.0f, .15f, 0.0f},
                           Pin{27000, .25f, 1.0f / 48.0f, .575f, 1.0f / 24.0f, {}},
                           Pin{81000, .5f, .0625f, 1.0f, .125f, {}},
                           Pin{135000, .75f, 5.0f / 48.0f, .575f, 5.0f / 24.0f, {}},
                           Pin{162000, 0.0f, .125f, .15f, .25f, {}},
                           Pin{648000, 0.0f, .5f, .15f, 1.0f, {}},
                           Pin{1296000, 0.0f, 0.0f, .15f, 0.0f, {}}}) {
        SCOPED_TRACE(pin.tick);
        Luminumbra::world::WorldClock(pin.tick - 1, pin.calendar).write_metadata(metadata);
        ASSERT_TRUE(P::WorldSaveService::save_metadata(metadata.dump(), save));
        ASSERT_TRUE(session.LoadWorld(id)) << session.GetWorldOpenError();
        const auto position = session.GetMetadata().spawnPoint;
        const auto plant = ClockTestPlant(session.GetRegistry(), position);
        const auto creature = ClockTestCreature(session.GetRegistry());
        ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
        const auto& clock = session.GetWorldClock();
        EXPECT_FLOAT_EQ(static_cast<float>(clock.solar_phase(pin.tick)), pin.day);
        EXPECT_FLOAT_EQ(
            static_cast<float>(clock.day_phase(pin.tick)),
            static_cast<float>(static_cast<double>(pin.tick % pin.calendar.dayLengthTicks) /
                               pin.calendar.dayLengthTicks));
        EXPECT_FLOAT_EQ(static_cast<float>(clock.year_phase(pin.tick)), pin.year);
        EXPECT_FLOAT_EQ(session.GetRegistry().get<C::CircadianComponent>(creature).activity,
                        luminumbra::ai::CircadianActivity(pin.day, false));
        const float spring = pin.year >= .25f ? pin.year - .25f : pin.year + .75f;
        const auto target = luminumbra::ai::MigrationTargetAt(spring);
        const auto& migration = session.GetRegistry().get<C::MigratoryComponent>(creature);
        EXPECT_FLOAT_EQ(migration.drive, luminumbra::ai::MigrationDriveAt(spring));
        const auto& creature_position =
            session.GetRegistry().get<C::TransformComponent>(creature).position;
        const float dx = target.x - creature_position.x;
        const float dz = target.z - creature_position.z;
        const float length = Luminumbra::DeterministicMath::Sqrt(dx * dx + dz * dz);
        EXPECT_NEAR(migration.wish_x, dx / length * migration.drive, 1e-6f);
        EXPECT_NEAR(migration.wish_z, dz / length * migration.drive, 1e-6f);
        const auto& needs = session.GetRegistry().get<C::NeedsComponent>(creature).needs;
        EXPECT_FLOAT_EQ(needs[0].pressure,
                        0.001f * luminumbra::ai::CircadianActivity(pin.day, false));
        const float season_stimulus =
            0.5f * (1.0f + Luminumbra::DeterministicMath::Sin(
                               spring * Luminumbra::DeterministicMath::kTwoPi));
        EXPECT_FLOAT_EQ(needs[1].pressure, 0.001f * season_stimulus);
        const auto sky = Luminumbra::Rendering::ComputeSeason(clock);
        EXPECT_FLOAT_EQ(sky.phase, spring);
        EXPECT_FLOAT_EQ(Luminumbra::Rendering::TimeOfDayFromWorldClock(clock),
                        pin.day >= .5f ? pin.day - .5f : pin.day + .5f);

        // Independent one-tick plant reference: pinned light/season values,
        // with the same terrain and rain samples but no session calendar math.
        F::PlantEnvSample env;
        auto* world = session.GetWorldSystem();
        const float height = world->GetTerrainHeightAt(position.x, position.z);
        switch (world->SurfaceVertexMaterial(position.x, position.z, height)) {
            case Luminumbra::MaterialType::Grass:
                env.soil_quality = .95f;
                break;
            case Luminumbra::MaterialType::Soil:
                env.soil_quality = .85f;
                break;
            case Luminumbra::MaterialType::Sand:
                env.soil_quality = .45f;
                break;
            case Luminumbra::MaterialType::Stone:
                env.soil_quality = .30f;
                break;
            default:
                env.soil_quality = .25f;
                break;
        }
        env.temperature = F::clamp01(
            F::clamp01(.60f - std::max(0.0f, height - static_cast<float>(Luminumbra::SEA_LEVEL)) *
                                  .00045f) +
            (pin.warmth - .5f) * .30f);
        env.light = pin.daylight;
        env.moisture =
            F::clamp01(.30f + session.GetWeatherSystem()->PrecipitationAt(position) * .70f);
        entt::registry expected;
        const auto reference = ClockTestPlant(expected, position);
        F::RunPlantGrowthSystemOnTick(expected, pin.tick, [&](const auto&) { return env; });
        const auto& actual = session.GetRegistry().get<C::PlantGrowthComponent>(plant);
        const auto& wanted = expected.get<C::PlantGrowthComponent>(reference);
        EXPECT_EQ(actual.growth_points, wanted.growth_points);
        EXPECT_EQ(actual.stress_points, wanted.stress_points);
        EXPECT_EQ(actual.last_tick, pin.tick);
    }
}
} // namespace

// Expected sessions contain the devel fixture's logical state without executing
// the persistence sequence under test. Ambient fields are computed in this build:
// their legacy FastNoise auto-dispatch differs between AVX2 and AVX-512, unlike
// terrain's pinned AVX2 path. Exact serialized fixture bytes remain the disk oracle.
void AdoptRegionGoldenChunk(GameSession& session, int x) {
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(x, 0, 0));
    chunk->set_state(Luminumbra::ChunkState::Idle);
    chunk->heightmap_data = {
        8.0f + static_cast<float>(x), 8.5f + static_cast<float>(x), 9.0f + static_cast<float>(x)};
    if (x == 0)
        chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
}

void ExpectSameWorldHash(GameSession& actual, GameSession& expected) {
    EXPECT_EQ(SessionWorldHash(actual), SessionWorldHash(expected))
        << "actual: " << SessionWorldSubHashes(actual).dump()
        << "\nexpected: " << SessionWorldSubHashes(expected).dump();
}

TEST(GameSessionHeadlessWorldTest, DisabledRegionsMatchDevelWorldHashAndExactSaveBytes) {
    const HeadlessRoot root;
    {
        const auto path = root.path() / "worlds/atlas/presets/default.json";
        std::ifstream input(path);
        auto preset = nlohmann::json::parse(input);
        input.close();
        preset["generation_params"]["terrain"]["island_mask_enabled"] = false;
        std::ofstream output(path);
        output << preset.dump(2);
    }
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, expected;
    for (auto* s : {&session, &expected}) {
        s->SetRootPath(root.root_string());
        s->SetJobSystem(&jobs);
        s->SetActiveRegionsEnabled(false);
        s->SetAetherStateEnabled(false);
        s->SetWaterHighResEnabled(false);
        s->SetWeatherRainEnabled(false);
        s->SetWeatherEventsEnabled(false);
        s->SetFiniteHydrologyEnabled(false);

        ASSERT_TRUE(s->CreateWorld("Region golden", "1337", "default"));
        ASSERT_TRUE(s->LoadWorldStateFrom(root.path() / "unmarked-snapshot"));
        s->GetWorldSystem()->SetWaterHydrology(false, 0, 0);
        ASSERT_NO_FATAL_FAILURE(AdoptRegionGoldenChunk(*s, 0));
    }
    // Disabled entry points must be inert, including otherwise invalid inputs.
    session.SetLocalPlayerSimulationPosition(Luminumbra::Vec3(20000, 0, 20000), true);
    session.PinActiveRegion({123, 45}, true);
    session.NotifyGroundObjectEdit(Luminumbra::Vec3(5000, 0, 5000));
    session.RecordActiveRegionWork({{123, 45}, UINT64_MAX, 0, 0, 0});
    for (int i = 0; i < 7; ++i) {
        ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
        ASSERT_EQ(expected.TickSimulation(expected.GetSimulationClock().fixed_dt()), 1u);
    }
    const auto save = root.path() / "golden-save";
    ASSERT_TRUE(session.SaveWorldStateTo(save));
    EXPECT_FALSE(fs::exists(save / "preset.json"));
    EXPECT_FALSE(fs::exists(save / "world_info.json"));
    ExpectSameWorldHash(session, expected);
    EXPECT_TRUE(session.GetActiveRegionLedger().records().empty());
    const auto fixture =
        fs::path(LUMINUMBRA_SOURCE_ROOT) / "test/fixtures/active-regions/devel-off";
    std::size_t files = 0;
    for (const auto& entry : fs::recursive_directory_iterator(save)) {
        if (!entry.is_regular_file())
            continue;
        ++files;
        EXPECT_EQ(ReadClockTestFile(entry.path()),
                  ReadClockTestFile(fixture / entry.path().filename()));
    }
    EXPECT_EQ(files, 2u);
    EXPECT_FALSE(fs::exists(P::WorldSaveService::active_regions_path(save)));
}

void SavePlantRosterThenMultiRegionEdit(GameSession& session, const fs::path& save) {
    ClockTestPlant(session.GetRegistry(), Luminumbra::Vec3(8, 20, 8));
    ASSERT_TRUE(session.SaveWorldStateTo(save));
    ASSERT_TRUE(P::WorldSaveService::has_world_save(save));
    ASSERT_TRUE(fs::exists(P::WorldSaveService::plant_entities_path(save)));
    ASSERT_FALSE(fs::exists(P::WorldSaveService::region_file_path(save, 0, 0)));
    for (const int x : {0, 32}) {
        ASSERT_NO_FATAL_FAILURE(AdoptRegionGoldenChunk(session, x));
    }
    ASSERT_TRUE(session.SaveWorldStateTo(save));
}

TEST(GameSessionHeadlessWorldTest,
     DisabledPlantRosterThenMultiRegionEditMatchesDevelHashAndExactSaveBytes) {
    const HeadlessRoot root;
    {
        const auto path = root.path() / "worlds/atlas/presets/default.json";
        std::ifstream input(path);
        auto preset = nlohmann::json::parse(input);
        input.close();
        preset["generation_params"]["terrain"]["island_mask_enabled"] = false;
        std::ofstream output(path);
        output << preset.dump(2);
    }
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, loaded, expected_live, expected_loaded;
    for (auto* s : {&session, &loaded, &expected_live, &expected_loaded}) {
        s->SetRootPath(root.root_string());
        s->SetJobSystem(&jobs);
        s->SetActiveRegionsEnabled(false);
        s->SetAetherStateEnabled(false);
        s->SetWaterHighResEnabled(false);
        s->SetWeatherRainEnabled(false);
        s->SetWeatherEventsEnabled(false);
        s->SetFiniteHydrologyEnabled(false);

        ASSERT_TRUE(s->CreateTransientWorld("Plant region golden", "1337", "default"));
        ASSERT_TRUE(s->LoadWorldStateFrom(root.path() / "unmarked-snapshot"));
    }
    for (auto* s : {&expected_live, &expected_loaded}) {
        ClockTestPlant(s->GetRegistry(), Luminumbra::Vec3(8, 20, 8));
        ASSERT_NO_FATAL_FAILURE(AdoptRegionGoldenChunk(*s, 0));
    }
    // Only the live session retains the clean, unsaved second region.
    ASSERT_NO_FATAL_FAILURE(AdoptRegionGoldenChunk(expected_live, 32));
    const auto save = root.path() / "plant-golden-save";
    ASSERT_NO_FATAL_FAILURE(SavePlantRosterThenMultiRegionEdit(session, save));
    EXPECT_FALSE(fs::exists(save / "preset.json"));
    EXPECT_FALSE(fs::exists(save / "world_info.json"));
    EXPECT_FALSE(session.ActiveRegionsEnabled());
    EXPECT_FALSE(fs::exists(P::WorldSaveService::region_file_path(save, 1, 0)));
    const auto fixture =
        fs::path(LUMINUMBRA_SOURCE_ROOT) / "test/fixtures/active-regions/devel-off-plant-edit";
    ExpectSameWorldHash(session, expected_live);
    ASSERT_TRUE(loaded.LoadWorldStateFrom(save));
    EXPECT_EQ(loaded.GetWorldSystem()->snapshot_streamed_chunks().size(), 1u);
    ExpectSameWorldHash(loaded, expected_loaded);
    std::size_t files = 0;
    for (const auto& entry : fs::recursive_directory_iterator(save)) {
        if (!entry.is_regular_file())
            continue;
        ++files;
        EXPECT_EQ(ReadClockTestFile(entry.path()),
                  ReadClockTestFile(fixture / entry.path().filename()));
    }
    EXPECT_EQ(files, 3u); // plant roster, dirty region and manifest; no clean region
}

TEST(GameSessionHeadlessWorldTest, AbsentLedgerAtRestoredTickAllowsEditAndPinBeforeSave) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original;
    original.SetRootPath(root.root_string());
    original.SetJobSystem(&jobs);
    original.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(original.CreateWorld("Absent ledger", "1337", "default"));
    for (int i = 0; i < 7; ++i) {
        ASSERT_EQ(original.TickSimulation(original.GetSimulationClock().fixed_dt()), 1u);
    }
    ASSERT_TRUE(original.SaveWorldState());
    for (const bool pin : {false, true}) {
        {
            P::SaveTransaction removed(
                original.GetWorldSaveDir(), 7, {}, &P::WorldSaveService::member_digest);
            ASSERT_TRUE(removed.begin(nullptr));
            ASSERT_TRUE(removed.protect({"chunks/region/active-regions.arl"}, nullptr));
            ASSERT_TRUE(
                fs::remove(P::WorldSaveService::active_regions_path(original.GetWorldSaveDir())));
            ASSERT_TRUE(removed.commit(nullptr));
        }
        GameSession loaded, restored;
        for (auto* s : {&loaded, &restored}) {
            s->SetRootPath(root.root_string());
            s->SetJobSystem(&jobs);
            s->SetActiveRegionsEnabled(true);
        }
        ASSERT_TRUE(loaded.LoadWorld(original.GetMetadata().worldId));
        ASSERT_EQ(loaded.GetWorldClock().tick(), 7u);
        ASSERT_EQ(loaded.GetActiveRegionLedger().tick(), 7u);
        ASSERT_TRUE(loaded.GetActiveRegionLedger().records().empty());
        EXPECT_EQ(loaded.GetRegionSchedule().tick, 0u); // no synthetic simulation tick
        const Luminumbra::world::RegionKey key{10, -3};
        if (pin)
            loaded.PinActiveRegion(key, true);
        else
            loaded.NotifyGroundObjectEdit(Luminumbra::Vec3(5200, 0, -1500));
        ASSERT_TRUE(loaded.IsSimulationTickBoundary());
        ASSERT_TRUE(loaded.SaveWorldState());
        ASSERT_TRUE(restored.LoadWorld(original.GetMetadata().worldId));
        EXPECT_EQ(restored.GetWorldClock().tick(), 7u);
        EXPECT_EQ(restored.GetActiveRegionLedger().canonical_bytes(),
                  loaded.GetActiveRegionLedger().canonical_bytes());
        EXPECT_EQ(restored.GetActiveRegionLedger().records().at(key).first_active, 7u);
        EXPECT_TRUE(restored.GetActiveRegionLedger().records().at(key).wake_pending);
        ASSERT_EQ(restored.TickSimulation(restored.GetSimulationClock().fixed_dt()), 1u);
        EXPECT_EQ(restored.GetActiveRegionLedger().records().at(key).last_ticked, 8u);
    }
}

void ExpectLatestRegionTelemetry(const GameSession& original,
                                 const GameSession& loaded,
                                 std::uint64_t tick) {
    const auto& original_samples = original.GetSimBudgetTelemetry().SamplesByStage();
    const auto& loaded_samples = loaded.GetSimBudgetTelemetry().SamplesByStage();
    for (std::size_t stage = 0; stage < original_samples.size(); ++stage) {
        if (original_samples[stage].empty()) {
            EXPECT_TRUE(loaded_samples[stage].empty());
            continue;
        }
        ASSERT_FALSE(loaded_samples[stage].empty());
        EXPECT_EQ(loaded_samples[stage].back().tick, tick);
        EXPECT_EQ(loaded_samples[stage].back().work, original_samples[stage].back().work);
    }
}

void SaveAndReopenRegionalSchedule(GameSession& original,
                                   GameSession& render_only,
                                   GameSession& loaded) {
    const auto before = SessionWorldHash(original);
    ASSERT_TRUE(original.SaveWorldState());
    EXPECT_EQ(SessionWorldHash(original), before);
    ASSERT_TRUE(render_only.SaveWorldState());
    EXPECT_EQ(
        ReadClockTestFile(P::WorldSaveService::active_regions_path(original.GetWorldSaveDir())),
        ReadClockTestFile(P::WorldSaveService::active_regions_path(render_only.GetWorldSaveDir())));
    ASSERT_TRUE(loaded.LoadWorld(original.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_EQ(loaded.GetActiveRegionLedger().encode(), original.GetActiveRegionLedger().encode());
    EXPECT_EQ(SessionWorldHash(loaded), before);
}

TEST(GameSessionHeadlessWorldTest, RegionScheduleAndWorldHashResumeAcrossBothHoldCounters) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original, uninterrupted, render_only, loaded;
    for (auto* session : {&original, &uninterrupted, &render_only, &loaded}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetActiveRegionsEnabled(true);
        session->SetRegionSchedulerConfig({10, 3, 2});
    }
    // Exercise the merged telemetry path through reduction, freeze, recovery,
    // and reload. Its observations must not become scheduler pressure or state.
    original.SetSimBudgetTelemetryEnabled(true);
    loaded.SetSimBudgetTelemetryEnabled(true);
    ASSERT_TRUE(original.CreateWorld("Region resume", "1337", "default"));
    ASSERT_TRUE(uninterrupted.CreateTransientWorld("Uninterrupted", "1337", "default"));
    ASSERT_TRUE(render_only.CreateWorld("Render variant", "1337", "default"));
    const Luminumbra::world::RegionKey far{40, -40};
    const Luminumbra::IVec3 coords(1280, 0, -1280);
    for (auto* session : {&original, &uninterrupted, &render_only}) {
        auto chunk = std::make_shared<Luminumbra::Chunk>(coords);
        chunk->set_state(Luminumbra::ChunkState::Idle);
        chunk->heightmap_data = {8.0f, 8.5f, 9.0f};
        chunk->mark_voxel_data_dirty();
        if (session == &render_only) {
            chunk->mesh_vertices.resize(3);
            chunk->mesh_indices = {0, 1, 2};
            chunk->water_mesh_vertices.resize(3);
            chunk->water_mesh_indices = {2, 1, 0};
            chunk->water_mesh_generated.store(true);
            chunk->water_mesh_dirty_ticks = 17;
        }
        ASSERT_TRUE(session->GetWorldSystem()->adopt_streamed_chunk(chunk));
    }
    for (auto* session : {&original, &uninterrupted, &render_only})
        session->PinActiveRegion(far, true);
    const auto dt = original.GetSimulationClock().fixed_dt();
    for (std::uint64_t tick = 1; tick <= 20; ++tick) {
        SCOPED_TRACE(tick);
        if (tick == 5) {
            // Change live simulation after the save at tick 3. At freeze, the
            // durable copy is stale and must lose to the dirty resident chunk.
            for (auto* session : {&original, &uninterrupted, &render_only, &loaded}) {
                // Bounded reopening leaves this far authority on disk until requested.
                // Adopt its exact durable bytes before editing the live authoritative object.
                ASSERT_TRUE(session->GetWorldSystem()->restore_authoritative_chunk(coords));
                const auto chunks = session->GetWorldSystem()->snapshot_streamed_chunks();
                ASSERT_EQ(chunks.size(), 1u);
                chunks.front()->heightmap_data[0] = 12.0f;
                chunks.front()->mark_voxel_data_dirty();
            }
        }
        for (auto* session : {&original, &uninterrupted, &render_only}) {
            session->RecordActiveRegionWork({far, tick <= 9 ? 11u : 10u, 0, 0, 0});
            ASSERT_EQ(session->TickSimulation(dt), 1u);
        }
        EXPECT_EQ(original.GetRegionSchedule(), uninterrupted.GetRegionSchedule());
        EXPECT_EQ(SessionWorldHash(original), SessionWorldHash(uninterrupted));
        EXPECT_EQ(original.GetRegionSchedule(), render_only.GetRegionSchedule());
        EXPECT_EQ(original.GetActiveRegionLedger().canonical_bytes(),
                  render_only.GetActiveRegionLedger().canonical_bytes());
        EXPECT_EQ(SessionWorldHash(original), SessionWorldHash(render_only));
        if (tick == 7) {
            ASSERT_EQ(original.GetActiveRegionLedger().records().at(far).state,
                      Luminumbra::world::RegionState::Frozen);
            EXPECT_EQ(original.GetActiveRegionLedger().records().at(far).frozen_digest,
                      uninterrupted.GetActiveRegionLedger().records().at(far).frozen_digest);
        }
        if (tick >= 4) {
            loaded.RecordActiveRegionWork({far, tick <= 9 ? 11u : 10u, 0, 0, 0});
            ASSERT_EQ(loaded.TickSimulation(dt), 1u);
            EXPECT_EQ(loaded.GetRegionSchedule(), original.GetRegionSchedule());
            EXPECT_EQ(loaded.GetActiveRegionLedger().canonical_bytes(),
                      original.GetActiveRegionLedger().canonical_bytes());
            EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(original));
            ASSERT_NO_FATAL_FAILURE(ExpectLatestRegionTelemetry(original, loaded, tick));
        }
        if (tick == 3 || tick == 11) {
            ASSERT_NO_FATAL_FAILURE(SaveAndReopenRegionalSchedule(original, render_only, loaded));
        }
    }
    EXPECT_EQ(loaded.GetSimulationTickCount(), 20u);
    const auto& combined =
        original.GetSimBudgetTelemetry().SamplesByStage()[static_cast<std::size_t>(
            luminumbra::simulation::SimBudgetStage::WindWeather)];
    ASSERT_EQ(combined.size(), 20u);
    for (const auto& sample : combined) {
        EXPECT_EQ(sample.work, 2u * 64u * 64u);
    }
    for (const auto& stage : uninterrupted.GetSimBudgetTelemetry().SamplesByStage()) {
        EXPECT_TRUE(stage.empty());
    }
}

namespace {
std::uint64_t Digest(const std::filesystem::path& save,
                     Luminumbra::world::RegionKey key,
                     std::vector<std::shared_ptr<Luminumbra::Chunk>> live,
                     std::vector<std::shared_ptr<Luminumbra::Chunk>> parked = {}) {
    return P::WorldSaveService::region_simulation_digest(
        save, key, P::RegionDigestInputs{std::move(live), std::move(parked), {}});
}
} // namespace

TEST(GameSessionHeadlessWorldTest, RegionDigestMergesLiveAndDurableSimulationOnly) {
    const HeadlessRoot root;
    // The content-v2 digest includes only authoritative edits. Keep the combined
    // live/durable regression by marking every simulated durable fixture as edited.
    Luminumbra::WorldStreamingState durable;
    const auto old = durable.get_or_create_chunk({0, 0, 0});
    old->heightmap_data = {1.0f};
    old->mark_voxel_data_dirty();
    const auto unloaded = durable.get_or_create_chunk({1, 0, 0});
    unloaded->heightmap_data = {2.0f};
    unloaded->mark_voxel_data_dirty();
    const auto other_region = durable.get_or_create_chunk({32, 0, 0});
    other_region->heightmap_data = {3.0f};
    other_region->mark_voxel_data_dirty();
    const auto save = root.path() / "digest";
    ASSERT_TRUE(P::WorldSaveService{}.save_world(durable, save));
    const auto live = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    live->heightmap_data = {4.0f};
    live->mark_voxel_data_dirty();
    const auto digest = P::WorldSaveService::region_simulation_digest(save, {0, 0}, {live});
    EXPECT_EQ(digest, P::WorldSaveService::region_simulation_digest({}, {0, 0}, {unloaded, live}));
    EXPECT_NE(digest, P::WorldSaveService::region_simulation_digest(save, {0, 0}, {}));
    // Both durable render data and live render data are outside the projection.
    unloaded->mesh_vertices.resize(3);
    unloaded->mesh_indices = {0, 1, 2};
    unloaded->water_mesh_generated.store(true);
    unloaded->water_mesh_dirty_ticks = 11;
    ASSERT_TRUE(P::WorldSaveService{}.save_world(durable, save));
    live->mesh_vertices.resize(3);
    live->mesh_indices = {2, 1, 0};
    live->water_mesh_dirty_ticks = 29;
    EXPECT_EQ(P::WorldSaveService::region_simulation_digest(save, {0, 0}, {live}), digest);
    for (const auto state : {Luminumbra::ChunkState::Meshing, Luminumbra::ChunkState::Ready}) {
        live->set_state(state);
        for (const bool collision : {true, false}) {
            live->has_collision.store(collision);
            EXPECT_EQ(P::WorldSaveService::region_simulation_digest(save, {0, 0}, {live}), digest);
        }
    }
    live->heightmap_data[0] = 5.0f;
    EXPECT_NE(P::WorldSaveService::region_simulation_digest(save, {0, 0}, {live}), digest);
    live->heightmap_data[0] = 4.0f;
    live->material_data = {1};
    EXPECT_NE(P::WorldSaveService::region_simulation_digest(save, {0, 0}, {live}), digest);
    live->material_data.clear();
    live->water_depth_mm = {1};
    EXPECT_NE(P::WorldSaveService::region_simulation_digest(save, {0, 0}, {live}), digest);
}

TEST(GameSessionHeadlessWorldTest, RegionDigestExcludesPristineAndLoadingChunks) {
    const auto empty = Digest({}, {0, 0}, {});
    const auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    chunk->heightmap_data = {1.0f};
    EXPECT_EQ(Digest({}, {0, 0}, {chunk}), empty);
    chunk->heightmap_data = {2.0f};
    EXPECT_EQ(Digest({}, {0, 0}, {chunk}), empty);
    chunk->mark_voxel_data_dirty();
    EXPECT_NE(Digest({}, {0, 0}, {chunk}), empty);
    chunk->set_state(Luminumbra::ChunkState::Loading);
    EXPECT_EQ(Digest({}, {0, 0}, {chunk}), empty);
    const auto other = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(32, 0, 0));
    other->heightmap_data = {3.0f};
    other->mark_voxel_data_dirty();
    EXPECT_EQ(Digest({}, {0, 0}, {other}), empty);
}

TEST(GameSessionHeadlessWorldTest, RegionDigestIsIndependentOfResidencyAndSaveHistory) {
    const HeadlessRoot root;
    const auto x = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    x->heightmap_data = {4.0f};
    x->mark_voxel_data_dirty();
    const auto live_only = Digest({}, {0, 0}, {x});
    EXPECT_EQ(Digest({}, {0, 0}, {}, {x}), live_only);
    Luminumbra::WorldStreamingState durable;
    const auto stored = durable.get_or_create_chunk({0, 0, 0});
    stored->heightmap_data = {4.0f};
    stored->mark_voxel_data_dirty();
    const auto save = root.path() / "digest-history";
    ASSERT_TRUE(P::WorldSaveService{}.save_world(durable, save));
    EXPECT_EQ(Digest(save, {0, 0}, {}), live_only);
    EXPECT_EQ(Digest(save, {0, 0}, {x}), live_only);
    x->clear_voxel_data_dirty();
    EXPECT_EQ(Digest(save, {0, 0}, {x}), live_only);
    EXPECT_EQ(Digest({}, {0, 0}, {x}), live_only);
    x->heightmap_data[0] = 5.0f;
    EXPECT_NE(Digest({}, {0, 0}, {x}), live_only);
}

TEST(GameSessionHeadlessWorldTest, RegionDigestPriorityIsLiveThenParkedThenDurable) {
    const HeadlessRoot root;
    Luminumbra::WorldStreamingState durable;
    const auto stored = durable.get_or_create_chunk({0, 0, 0});
    stored->heightmap_data = {1.0f};
    stored->mark_voxel_data_dirty();
    const auto save = root.path() / "digest-priority";
    ASSERT_TRUE(P::WorldSaveService{}.save_world(durable, save));
    const auto parked = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    parked->heightmap_data = {2.0f};
    parked->mark_voxel_data_dirty();
    const auto live = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    live->heightmap_data = {3.0f};
    live->mark_voxel_data_dirty();
    EXPECT_EQ(Digest(save, {0, 0}, {live}, {parked}), Digest({}, {0, 0}, {live}));
    EXPECT_EQ(Digest(save, {0, 0}, {}, {parked}), Digest({}, {0, 0}, {}, {parked}));
    const auto durable_only = Digest(save, {0, 0}, {});
    EXPECT_NE(Digest(save, {0, 0}, {}, {parked}), durable_only);
    EXPECT_NE(Digest({}, {0, 0}, {live}), Digest({}, {0, 0}, {parked}));
    EXPECT_NE(Digest({}, {0, 0}, {live}), durable_only);
    EXPECT_NE(Digest({}, {0, 0}, {parked}), durable_only);
}

TEST(GameSessionHeadlessWorldTest, RegionDigestIgnoresUnflaggedDurableRecords) {
    const HeadlessRoot root;
    Luminumbra::WorldStreamingState durable;
    durable.get_or_create_chunk({0, 0, 0})->heightmap_data = {1.0f};
    const auto save = root.path() / "digest-unflagged";
    ASSERT_TRUE(P::WorldSaveService{}.save_world(durable, save));
    EXPECT_EQ(Digest(save, {0, 0}, {}), Digest({}, {0, 0}, {}));
}

TEST(GameSessionHeadlessWorldTest, RegionDigestIgnoresRenderAndResidencyState) {
    const auto live = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    live->heightmap_data = {4.0f};
    live->mark_voxel_data_dirty();
    const auto digest = Digest({}, {0, 0}, {live});
    live->mesh_vertices.resize(3);
    live->mesh_indices = {2, 1, 0};
    live->water_mesh_dirty_ticks = 29;
    EXPECT_EQ(Digest({}, {0, 0}, {live}), digest);
    for (const auto state : {Luminumbra::ChunkState::Meshing, Luminumbra::ChunkState::Ready}) {
        live->set_state(state);
        for (const bool collision : {true, false}) {
            live->has_collision.store(collision);
            EXPECT_EQ(Digest({}, {0, 0}, {live}), digest);
        }
    }
    live->heightmap_data[0] = 5.0f;
    EXPECT_NE(Digest({}, {0, 0}, {live}), digest);
    live->heightmap_data[0] = 4.0f;
    live->material_data = {1};
    EXPECT_NE(Digest({}, {0, 0}, {live}), digest);
    live->material_data.clear();
    live->water_depth_mm = {1};
    EXPECT_NE(Digest({}, {0, 0}, {live}), digest);
}

TEST(GameSessionHeadlessWorldTest, RegionDigestIgnoresLoadTimeCollisionNormalization) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, loaded;
    for (auto* s : {&session, &loaded}) {
        s->SetRootPath(root.root_string());
        s->SetJobSystem(&jobs);
        s->SetActiveRegionsEnabled(true);
    }
    ASSERT_TRUE(session.CreateWorld("Collision normalization", "1337", "default"));
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    chunk->set_state(Luminumbra::ChunkState::Meshing);
    chunk->has_collision.store(true);
    chunk->heightmap_data = {8.0f};
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
    const auto digest = Digest({}, {0, 0}, {chunk});
    ASSERT_TRUE(session.SaveWorldState());
    ASSERT_TRUE(loaded.LoadWorld(session.GetMetadata().worldId));
    ASSERT_TRUE(loaded.GetWorldSystem()->restore_authoritative_chunk({0, 0, 0}));
    const auto chunks = loaded.GetWorldSystem()->snapshot_streamed_chunks();
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_FALSE(chunks.front()->has_collision.load());
    EXPECT_EQ(chunks.front()->get_state(), Luminumbra::ChunkState::Idle);
    EXPECT_EQ(Digest({}, {0, 0}, chunks), digest);
    EXPECT_EQ(Digest(session.GetWorldSaveDir(), {0, 0}, {}), digest);
}

TEST(GameSessionHeadlessWorldTest, FirstEnabledChunkSaveIncludesCleanResidentRegions) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, loaded;
    for (auto* s : {&session, &loaded}) {
        s->SetRootPath(root.root_string());
        s->SetJobSystem(&jobs);
        s->SetActiveRegionsEnabled(true);
    }
    ASSERT_TRUE(session.CreateWorld("Initial regions", "1337", "default"));
    const auto save = session.GetWorldSaveDir();
    EXPECT_TRUE(P::WorldSaveService::has_world_save(save));
    EXPECT_FALSE(P::WorldSaveService::has_chunk_snapshot(save));
    for (const int x : {0, 32}) {
        auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(x, 0, 0));
        chunk->set_state(Luminumbra::ChunkState::Idle);
        chunk->heightmap_data = {8.0f + static_cast<float>(x)};
        if (x == 0)
            chunk->mark_voxel_data_dirty();
        ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
    }
    const auto before = SessionWorldHash(session);
    ASSERT_TRUE(session.SaveWorldState());
    EXPECT_TRUE(P::WorldSaveService::has_chunk_snapshot(save));
    EXPECT_TRUE(fs::exists(P::WorldSaveService::region_file_path(save, 0, 0)));
    EXPECT_TRUE(fs::exists(P::WorldSaveService::region_file_path(save, 1, 0)));
    ASSERT_TRUE(loaded.LoadWorld(session.GetMetadata().worldId));
    EXPECT_EQ(loaded.GetWorldSystem()->durable_chunk_inventory().size(), 1u);
    for (const int x : {0, 32}) {
        const Luminumbra::IVec3 coords(x, 0, 0);
        if (!loaded.GetWorldSystem()->find_streamed_chunk(coords)) {
            const auto exact = P::WorldSaveService::read_chunk(save, coords, true);
            ASSERT_EQ(exact.status, P::WorldSaveService::ChunkReadStatus::Found);
            ASSERT_TRUE(loaded.GetWorldSystem()->adopt_streamed_chunk(exact.chunk));
        }
    }
    EXPECT_EQ(loaded.GetWorldSystem()->snapshot_streamed_chunks().size(), 2u);
    EXPECT_EQ(SessionWorldHash(loaded), before);
}

TEST(GameSessionHeadlessWorldTest, EmptyReplicatedAnchorsNeverUseLocalSpawnFallback) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession server;
    server.SetRootPath(root.root_string());
    server.SetJobSystem(&jobs);
    server.SetActiveRegionsEnabled(true);
    server.SetRegionSchedulerConfig({0, 1, 1});
    ASSERT_TRUE(server.CreateTransientWorld("Empty server", "1337", "default"));
    const auto spawn = server.GetMetadata().spawnPoint;
    ASSERT_EQ(server.GetActiveRegionLedger().local_anchor(),
              std::optional<Luminumbra::Vec3>(spawn));
    server.SetReplicatedSimulationAnchors({});
    const auto dt = server.GetSimulationClock().fixed_dt();
    ASSERT_EQ(server.TickSimulation(dt), 1u);
    EXPECT_TRUE(server.GetActiveRegionLedger().records().empty());
    EXPECT_TRUE(server.GetRegionSchedule().due.empty());
    server.SetReplicatedSimulationAnchors({spawn});
    ASSERT_EQ(server.TickSimulation(dt), 1u);
    const auto key = Luminumbra::world::ActiveRegionLedger::region_at(spawn);
    EXPECT_EQ(server.GetActiveRegionLedger().records().at(key).last_proximity, 2u);
    server.SetReplicatedSimulationAnchors({});
    server.RecordActiveRegionWork({key, 1, 0, 0, 0});
    ASSERT_EQ(server.TickSimulation(dt), 1u);
    for (const auto& [region, record] : server.GetActiveRegionLedger().records()) {
        (void)region;
        EXPECT_EQ(record.last_proximity, 2u);
    }
    ASSERT_EQ(server.GetRegionSchedule().transitions.size(), 1u);
    EXPECT_EQ(server.GetRegionSchedule().transitions.front().to,
              Luminumbra::world::RegionState::Reduced);
}

TEST(GameSessionHeadlessWorldTest, QueuedRegionWorkRefusesSaveUntilTickCompletes) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, loaded;
    for (auto* s : {&session, &loaded}) {
        s->SetRootPath(root.root_string());
        s->SetJobSystem(&jobs);
        s->SetActiveRegionsEnabled(true);
        s->SetRegionSchedulerConfig({0, 2, 1});
    }
    ASSERT_TRUE(session.CreateWorld("Queued work", "1337", "default"));
    const Luminumbra::world::RegionKey far{40, -40};
    session.PinActiveRegion(far, true);
    const auto dt = session.GetSimulationClock().fixed_dt();
    ASSERT_EQ(session.TickSimulation(dt), 1u); // consume pin wake
    session.RecordActiveRegionWork({far, 1, 0, 0, 0});
    ASSERT_EQ(session.TickSimulation(dt), 1u);
    ASSERT_EQ(session.GetActiveRegionLedger().records().at(far).over_hold, 1u);
    ASSERT_TRUE(session.SaveWorldState());
    const auto save = session.GetWorldSaveDir();
    const auto metadata = ReadClockTestFile(save / "world_info.json");
    const auto ledger = ReadClockTestFile(P::WorldSaveService::active_regions_path(save));
    session.RecordActiveRegionWork({far, 1, 0, 0, 0});
    EXPECT_FALSE(session.IsSimulationTickBoundary());
    EXPECT_EQ(session.TickSimulation(0.0), 0u);
    EXPECT_FALSE(session.IsSimulationTickBoundary());
    EXPECT_FALSE(session.SaveWorld());
    EXPECT_FALSE(session.SaveWorldState());
    const auto alternate = root.path() / "incomplete";
    EXPECT_FALSE(session.SaveWorldStateTo(alternate));
    EXPECT_FALSE(fs::exists(alternate));
    EXPECT_EQ(ReadClockTestFile(save / "world_info.json"), metadata);
    EXPECT_EQ(ReadClockTestFile(P::WorldSaveService::active_regions_path(save)), ledger);
    ASSERT_EQ(session.TickSimulation(dt), 1u);
    EXPECT_TRUE(session.IsSimulationTickBoundary());
    EXPECT_EQ(session.GetActiveRegionLedger().records().at(far).state,
              Luminumbra::world::RegionState::Reduced);
    ASSERT_TRUE(session.SaveWorldState());
    ASSERT_TRUE(loaded.LoadWorld(session.GetMetadata().worldId));
    EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(session));
    for (auto* s : {&session, &loaded}) {
        s->RecordActiveRegionWork({far, 1, 0, 0, 0});
        ASSERT_EQ(s->TickSimulation(dt), 1u);
    }
    EXPECT_EQ(loaded.GetRegionSchedule(), session.GetRegionSchedule());
    EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(session));
}

TEST(GameSessionHeadlessWorldTest, IdleRetentionMatchesWorkAndAllResidentOwnership) {
    using Luminumbra::world::ActiveRegionLedger;
    using Luminumbra::world::RegionKey;
    using Luminumbra::world::RegionWork;
    using Luminumbra::world::WorldClock;
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Idle retention", "1337", "default"));
    ASSERT_TRUE(session.SaveWorldState());

    const RegionKey pending{40, -40}, duplicate{41, -40}, zero_count{42, -40};
    const RegionKey resident{43, -40}, pristine{44, -40}, unowned{45, -40};
    const RegionKey loading{46, -40}, pristine_loading{47, -40};
    const std::array keys{
        pending, duplicate, zero_count, resident, pristine, unowned, loading, pristine_loading};
    ActiveRegionLedger staged;
    staged.set_local_anchor(session.GetMetadata().spawnPoint);
    for (const auto key : keys) {
        staged.pin(key, true, WorldClock{});
        staged.pin(key, false, WorldClock{});
    }
    // Stage just before retention without advancing thousands of simulation ticks.
    const WorldClock before_idle(ActiveRegionLedger::kIdleEvictionTicks - 1);
    staged.schedule(before_idle, std::span<const Luminumbra::Vec3>{});
    const auto save = session.GetWorldSaveDir();
    auto metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    before_idle.write_metadata(metadata);
    ASSERT_TRUE(
        P::WorldSaveService::save_metadata_and_active_regions(metadata.dump(), staged, save));
    ASSERT_TRUE(session.LoadWorldStateFrom(save)) << session.GetWorldOpenError();
    ASSERT_EQ(session.GetActiveRegionLedger().records().size(), keys.size());
    session.SetReplicatedSimulationAnchors({});

    for (const auto key : {resident, pristine, loading, pristine_loading}) {
        auto chunk =
            std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(key.x * 32, 0, key.z * 32));
        chunk->set_state(key == loading || key == pristine_loading ? Luminumbra::ChunkState::Loading
                                                                   : Luminumbra::ChunkState::Idle);
        chunk->heightmap_data = {8.0f};
        if (key == resident || key == loading)
            chunk->mark_voxel_data_dirty();
        ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
        // Adoption settles populated Loading chunks; explicitly exercise an in-flight resident.
        if (key == loading || key == pristine_loading)
            chunk->set_state(Luminumbra::ChunkState::Loading);
        // Residency protects the record regardless of provenance or lifecycle state.
        ASSERT_FALSE(session.GetActiveRegionLedger().records().at(key).edited);
    }
    const std::array work{RegionWork{pending, 3, 0, 0, 0},
                          RegionWork{duplicate, 1, 0, 0, 0},
                          RegionWork{duplicate, 2, 0, 0, 0},
                          RegionWork{zero_count, 0, 0, 0, 0},
                          RegionWork{zero_count, 0, 0, 0, 0}};
    for (const auto& entry : work)
        session.RecordActiveRegionWork(entry);
    auto expected = session.GetActiveRegionLedger();
    const std::array owned_keys{
        pending, duplicate, zero_count, resident, pristine, loading, pristine_loading};
    const auto expected_schedule = expected.schedule(
        WorldClock(ActiveRegionLedger::kIdleEvictionTicks),
        std::span<const Luminumbra::Vec3>{},
        work,
        {},
        [&owned_keys](RegionKey key) {
            return std::find(owned_keys.begin(), owned_keys.end(), key) != owned_keys.end();
        });
    const auto dt = session.GetSimulationClock().fixed_dt();
    ASSERT_EQ(session.TickSimulation(dt), 1u);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_EQ(session.GetRegionSchedule(), expected_schedule);
    EXPECT_EQ(session.GetActiveRegionLedger().canonical_bytes(), expected.canonical_bytes());
    EXPECT_EQ(session.GetActiveRegionLedger().encode(), expected.encode());
    ASSERT_EQ(session.GetActiveRegionLedger().records().size(), owned_keys.size());
    for (const auto key : owned_keys) {
        EXPECT_TRUE(session.GetActiveRegionLedger().records().contains(key));
    }
    EXPECT_FALSE(session.GetActiveRegionLedger().records().contains(unowned));

    // Consumed work must stop owning records on the next scheduling call.
    const auto next_schedule = expected.schedule(
        WorldClock(ActiveRegionLedger::kIdleEvictionTicks + 1),
        std::span<const Luminumbra::Vec3>{},
        {},
        {},
        [resident, pristine, loading, pristine_loading](RegionKey key) {
            return key == resident || key == pristine || key == loading || key == pristine_loading;
        });
    ASSERT_EQ(session.TickSimulation(dt), 1u);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_EQ(session.GetRegionSchedule(), next_schedule);
    EXPECT_EQ(session.GetActiveRegionLedger().canonical_bytes(), expected.canonical_bytes());
    EXPECT_EQ(session.GetActiveRegionLedger().encode(), expected.encode());
    ASSERT_EQ(session.GetActiveRegionLedger().records().size(), 4u);
    for (const auto key : {resident, pristine, loading, pristine_loading}) {
        EXPECT_TRUE(session.GetActiveRegionLedger().records().contains(key));
    }
}

TEST(GameSessionHeadlessWorldTest, WalkingAnchorSurvivesNoclipAndReloadWithoutCameraInputs) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession walking, noclip, loaded;
    for (auto* session : {&walking, &noclip, &loaded}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetActiveRegionsEnabled(true);
    }
    ASSERT_TRUE(walking.CreateTransientWorld("Walking", "1337", "default"));
    ASSERT_TRUE(noclip.CreateWorld("Noclip", "1337", "default"));
    const Luminumbra::Vec3 feet(-768, 30, 768);
    walking.SetLocalPlayerSimulationPosition(feet, true);
    noclip.SetLocalPlayerSimulationPosition(feet, true);
    for (int tick = 0; tick < 5; ++tick) {
        noclip.SetLocalPlayerSimulationPosition(Luminumbra::Vec3(10000 + tick * 1000, 500, -20000),
                                                false);
        ASSERT_EQ(walking.TickSimulation(walking.GetSimulationClock().fixed_dt()), 1u);
        ASSERT_EQ(noclip.TickSimulation(noclip.GetSimulationClock().fixed_dt()), 1u);
        EXPECT_EQ(walking.GetRegionSchedule(), noclip.GetRegionSchedule());
        EXPECT_EQ(SessionWorldHash(walking), SessionWorldHash(noclip));
    }
    ASSERT_TRUE(noclip.SaveWorldState());
    ASSERT_TRUE(loaded.LoadWorld(noclip.GetMetadata().worldId));
    EXPECT_EQ(loaded.GetActiveRegionLedger().local_anchor(), std::optional<Luminumbra::Vec3>(feet));
    EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(noclip));
    for (auto* session : {&walking, &noclip, &loaded}) {
        ASSERT_EQ(session->TickSimulation(session->GetSimulationClock().fixed_dt()), 1u);
    }
    EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(walking));
    // Replicated avatars replace the single-player anchor for activation.
    loaded.SetReplicatedSimulationAnchors({Luminumbra::Vec3(10000, 0, 10000)});
    ASSERT_EQ(loaded.TickSimulation(loaded.GetSimulationClock().fixed_dt()), 1u);
    EXPECT_EQ(loaded.GetActiveRegionLedger().records().at({-2, 1}).last_proximity, 6u);
    EXPECT_EQ(loaded.GetActiveRegionLedger().records().at({19, 19}).last_proximity, 7u);
}

TEST(GameSessionHeadlessWorldTest, LedgerFileAbsenceCorruptionFutureAndDisabledRefusals) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner;
    owner.SetRootPath(root.root_string());
    owner.SetJobSystem(&jobs);
    owner.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(owner.CreateWorld("Ledger refusal", "1337", "default"));
    ASSERT_EQ(owner.TickSimulation(owner.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_TRUE(owner.SaveWorldState());
    const auto save = owner.GetWorldSaveDir();
    const auto path = P::WorldSaveService::active_regions_path(save);
    const auto good = ReadClockTestFile(path);
    ASSERT_TRUE(P::WorldSaveService::validate_save(save));
    for (const bool future : {false, true}) {
        auto bytes = good;
        if (future)
            bytes[4] = 3;
        else
            bytes[8] ^= 1;
        std::ofstream(path, std::ios::binary) << bytes;
        std::vector<std::string> errors;
        EXPECT_FALSE(P::WorldSaveService::validate_save(save, &errors));
        ASSERT_FALSE(errors.empty());
        EXPECT_EQ(errors.front(),
                  future ? Luminumbra::world::ActiveRegionLedger::kFutureMessage
                         : Luminumbra::world::ActiveRegionLedger::kCorruptMessage);
        EXPECT_FALSE(owner.SaveWorldState());
        EXPECT_EQ(ReadClockTestFile(path), bytes);
        GameSession refused;
        refused.SetRootPath(root.root_string());
        refused.SetJobSystem(&jobs);
        refused.SetActiveRegionsEnabled(true);
        EXPECT_FALSE(refused.LoadWorld(owner.GetMetadata().worldId));
        EXPECT_EQ(refused.GetWorldOpenError(), errors.front());
    }
    std::ofstream(path, std::ios::binary) << good;
    GameSession disabled;
    disabled.SetActiveRegionsEnabled(false); // Exercise the incompatible-config refusal.
    disabled.SetRootPath(root.root_string());
    disabled.SetJobSystem(&jobs);
    EXPECT_FALSE(disabled.LoadWorld(owner.GetMetadata().worldId));
    EXPECT_NE(disabled.GetWorldOpenError().find("Incompatible configuration"), std::string::npos);
    {
        P::SaveTransaction removed(save, 1, {}, &P::WorldSaveService::member_digest);
        ASSERT_TRUE(removed.begin(nullptr));
        ASSERT_TRUE(removed.protect({"chunks/region/active-regions.arl"}, nullptr));
        ASSERT_TRUE(fs::remove(path));
        ASSERT_TRUE(removed.commit(nullptr));
    }
    Luminumbra::world::ActiveRegionLedger empty;
    EXPECT_TRUE(P::WorldSaveService::load_active_regions(empty, save));
    EXPECT_TRUE(empty.records().empty());
    EXPECT_TRUE(P::WorldSaveService::validate_save(save));
    GameSession legacy;
    legacy.SetRootPath(root.root_string());
    legacy.SetJobSystem(&jobs);
    legacy.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(legacy.LoadWorld(owner.GetMetadata().worldId));
    EXPECT_TRUE(legacy.GetActiveRegionLedger().records().empty());
    EXPECT_EQ(legacy.GetSimulationTickCount(), 1u);
}

TEST(GameSessionHeadlessWorldTest, VoxelGroundObjectAndPinInputsActivateOnlyTheirRegions) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateTransientWorld("Edit activation", "1337", "default"));
    auto* world = session.GetWorldSystem();
    // Adopt an all-air lattice at the far edit, without render-driven activation.
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(320, 0, -320));
    chunk->set_state(Luminumbra::ChunkState::Idle);
    chunk->sdf_data.assign(std::size_t{17} * 17 * 17, 1.0f);
    ASSERT_TRUE(world->adopt_streamed_chunk(chunk));
    ASSERT_GT(world->EditTerrainVoxel(Luminumbra::Vec3(5128, 8, -5112), 1, true, nullptr), 0);
    EXPECT_TRUE(session.GetActiveRegionLedger().records().at({10, -10}).edited);
    session.NotifyGroundObjectEdit(Luminumbra::Vec3(-5121, 0, 5121));
    EXPECT_TRUE(session.GetActiveRegionLedger().records().at({-11, 10}).edited);
    session.PinActiveRegion({20, 20}, true);
    EXPECT_TRUE(session.GetActiveRegionLedger().records().at({20, 20}).pinned);
    EXPECT_EQ(session.GetActiveRegionLedger().records().size(), 3u);
}

TEST(RegionScheduleTrace, FreshDestinationAndInvalidInputsPreserveTheOutput) {
    using namespace Luminumbra::Replay;
    const auto root = Luminumbra::TestSupport::CreateUniqueTempDirectory("region-trace-input-");
    const auto replay = root / "fresh" / "capture.replay";
    auto path = replay;
    path += ".regions.json";
    const RegionScheduleTrace original{{1, "0123456789abcdef"}};
    std::string error;
    ASSERT_TRUE(WriteRegionScheduleTrace(replay, original, error)) << error;
    RegionScheduleTrace loaded;
    ASSERT_TRUE(ReadRegionScheduleTrace(replay, 1, loaded, error)) << error;
    EXPECT_EQ(loaded, original);
    fs::remove(path);
    fs::create_directory(path);
    EXPECT_FALSE(ReadRegionScheduleTrace(replay, 1, loaded, error));
    EXPECT_EQ(loaded, original);
    fs::remove(path);
    {
        std::ofstream output(path);
        output << "{";
    }
    EXPECT_FALSE(ReadRegionScheduleTrace(replay, 1, loaded, error));
    EXPECT_EQ(loaded, original);
    fs::remove(path);
    EXPECT_FALSE(ReadRegionScheduleTrace(replay, 1, loaded, error));
    EXPECT_EQ(error, "Missing region schedule trace.");
    EXPECT_EQ(loaded, original);
    std::error_code cleanup_error;
    fs::remove_all(root, cleanup_error);
}

TEST(GameSessionHeadlessWorldTest, ReplayScheduleCompanionRoundTripsAndRefusesInvalidTraces) {
    const HeadlessRoot root;
    const auto path = root.path() / "recording.lrec";
    using namespace Luminumbra::Replay;
    const RegionScheduleTrace trace{{1, "0123456789abcdef"}, {2, "fedcba9876543210"}};
    RegionScheduleTrace restored;
    std::string error;
    EXPECT_FALSE(ReadRegionScheduleTrace(path, 2, restored, error));
    EXPECT_EQ(error, "Missing region schedule trace.");
    ASSERT_TRUE(WriteRegionScheduleTrace(path, trace, error));
    ASSERT_TRUE(ReadRegionScheduleTrace(path, 2, restored, error));
    EXPECT_EQ(restored, trace);
    EXPECT_FALSE(ReadRegionScheduleTrace(path, 3, restored, error));
    EXPECT_EQ(error, "Corrupt region schedule trace.");
    const auto companion = path.string() + ".regions.json";
    const auto good = nlohmann::json::parse(ReadClockTestFile(companion));
    auto value = good;
    value["ticks"][0]["digest"] = "0000000000000000";
    std::ofstream(companion, std::ios::binary) << value.dump();
    EXPECT_FALSE(ReadRegionScheduleTrace(path, 2, restored, error));
    EXPECT_EQ(error, "Corrupt region schedule trace.");
    EXPECT_EQ(restored, trace);
    value = good;
    value["schema"] = "luminumbra.region_schedule_trace.v2";
    std::ofstream(companion, std::ios::binary) << value.dump();
    EXPECT_FALSE(ReadRegionScheduleTrace(path, 2, restored, error));
    EXPECT_EQ(error, "Unsupported future region schedule trace version.");
    EXPECT_FALSE(WriteRegionScheduleTrace(path, {{2, "0123456789abcdef"}}, error));
}

TEST(GameSessionHeadlessWorldTest, InvalidHostInputsNeverThrowAndAreCounted) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    session.SetRegionSchedulerConfig({0, 2, 1});
    ASSERT_TRUE(session.CreateTransientWorld("Invalid inputs", "1337", "default"));
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    EXPECT_NO_THROW(session.SetLocalPlayerSimulationPosition(Luminumbra::Vec3(nan, 0, 0), true));
    EXPECT_NO_THROW(session.NotifyGroundObjectEdit(Luminumbra::Vec3(inf, 0, 0)));
    EXPECT_NO_THROW(session.PinActiveRegion({40000, 0}, true));
    EXPECT_EQ(session.GetRegionInputRejections().anchors, 1u);
    EXPECT_EQ(session.GetRegionInputRejections().edits, 1u);
    EXPECT_EQ(session.GetRegionInputRejections().pins, 1u);
    EXPECT_FALSE(session.GetRegionSchedulerFault().has_value());
    EXPECT_TRUE(session.GetActiveRegionLedger().records().empty());
    session.SetReplicatedSimulationAnchors(
        {Luminumbra::Vec3(nan, 0, 0), Luminumbra::Vec3(256, 0, 256)});
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    EXPECT_EQ(session.GetRegionSchedule().rejected.anchors, 1u);
    EXPECT_FALSE(session.GetRegionSchedulerFault().has_value());
    EXPECT_EQ(session.GetActiveRegionLedger().records().count({0, 0}), 1u);
    const Luminumbra::Vec3 walking(256, 0, 256);
    session.SetLocalPlayerSimulationPosition(walking, true);
    session.SetLocalPlayerSimulationPosition(Luminumbra::Vec3(nan, 0, 0), true);
    EXPECT_EQ(session.GetActiveRegionLedger().local_anchor(),
              std::optional<Luminumbra::Vec3>(walking));
    EXPECT_EQ(session.GetRegionInputRejections().anchors, 2u);
}

namespace {
constexpr Luminumbra::world::RegionKey kFaultRegion{40, -40};

// Arms a scheduler fault at tick k. The pinned far region gets work over the
// limit in completed ticks k-2 and k-1: Active->Reduced at k-1, then Frozen at k,
// where the durable digest provider reads the corrupt region file and throws.
void ArmSchedulerFault(GameSession& session,
                       const HeadlessRoot& root,
                       JobSystem& jobs,
                       std::uint64_t k) {
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    session.SetRegionSchedulerConfig({1, 1, 1});
    ASSERT_TRUE(session.CreateWorld("Scheduler fault", "1337", "default"));
    session.PinActiveRegion(kFaultRegion, true);
    const auto region = P::WorldSaveService::region_file_path(
        session.GetWorldSaveDir(), kFaultRegion.x, kFaultRegion.z);
    fs::create_directories(region.parent_path());
    std::ofstream(region, std::ios::binary) << "not a region file";
    session.GetSimulationEventBus().subscribe([&session, k](const auto& event) {
        if (event.tick == k - 2 || event.tick == k - 1)
            session.RecordActiveRegionWork({kFaultRegion, 5, 0, 0, 0});
    });
    for (std::uint64_t t = 1; t <= k + 1; ++t)
        session.GetSimulationEventBus().publish(t, "work", "");
}

// The corrupt durable file only exists to make the digest provider throw. A
// save validates every region file, so restore a clean miss before saving.
void RemoveFaultRegionFile(const GameSession& session) {
    std::error_code error;
    EXPECT_TRUE(fs::remove(P::WorldSaveService::region_file_path(
                               session.GetWorldSaveDir(), kFaultRegion.x, kFaultRegion.z),
                           error));
    EXPECT_FALSE(error) << error.message();
}

// The session clock produces at most two ticks per frame. Single-tick frames run
// ticks 1..start-1, then one two-tick frame starts at tick `start` (k or k-1), so
// the fault lands on the first or the second tick of that frame.
void RunSchedulerFaultCase(std::uint64_t k, std::uint64_t start) {
    SCOPED_TRACE("k=" + std::to_string(k) + " start=" + std::to_string(start));
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, loaded;
    ASSERT_NO_FATAL_FAILURE(ArmSchedulerFault(session, root, jobs, k));
    loaded.SetRootPath(root.root_string());
    loaded.SetJobSystem(&jobs);
    loaded.SetActiveRegionsEnabled(true);
    loaded.SetRegionSchedulerConfig({1, 1, 1});
    const auto dt = session.GetSimulationClock().fixed_dt();
    for (std::uint64_t t = 1; t < start; ++t) {
        ASSERT_EQ(session.TickSimulation(dt), 1u);
    }
    ASSERT_FALSE(session.GetRegionSchedulerFault().has_value());
    // Half a tick of slack keeps the clock from rounding the frame down. A fault
    // on the first tick of the frame stops the second tick from running.
    ASSERT_EQ(session.TickSimulation(2.5 * dt), k - start + 1);
    const auto fault = session.GetRegionSchedulerFault();
    ASSERT_TRUE(fault.has_value());
    if (fault.has_value()) {
        const auto& snapshot = fault.value();
        EXPECT_EQ(snapshot.tick, k);
        EXPECT_FALSE(snapshot.message.empty());
    }
    EXPECT_EQ(session.GetSimulationTickCount(), k);
    EXPECT_LT(session.GetActiveRegionLedger().tick(), k);
    EXPECT_EQ(session.GetActiveRegionLedger().header_tick(), k);
    EXPECT_TRUE(session.IsSimulationTickBoundary());
    EXPECT_EQ(session.TickSimulation(dt), 0u);
    EXPECT_EQ(session.GetSimulationTickCount(), k);
    const auto save = session.GetWorldSaveDir();
    RemoveFaultRegionFile(session);
    ASSERT_TRUE(session.SaveWorldState());
    ASSERT_TRUE(session.SaveWorld());
    ASSERT_TRUE(P::WorldSaveService::validate_save(save));
    ASSERT_TRUE(loaded.LoadWorld(session.GetMetadata().worldId));
    EXPECT_EQ(loaded.GetSimulationTickCount(), k);
    EXPECT_FALSE(loaded.GetRegionSchedulerFault().has_value());
    const auto saved_hashes = SessionWorldSubHashes(session);
    const auto loaded_hashes = SessionWorldSubHashes(loaded);
    for (const auto& [field, hash] : saved_hashes.items())
        EXPECT_EQ(loaded_hashes.at(field), hash) << field;
    EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(session));
}
} // namespace

TEST(GameSessionHeadlessWorldTest, SchedulerFaultStopsTheFrameAndLeavesASavableConsistentSession) {
    for (const auto& [k, start] : {std::pair<std::uint64_t, std::uint64_t>{3, 3},
                                   std::pair<std::uint64_t, std::uint64_t>{3, 2},
                                   std::pair<std::uint64_t, std::uint64_t>{4, 4}}) {
        ASSERT_NO_FATAL_FAILURE(RunSchedulerFaultCase(k, start));
    }
}

TEST(GameSessionHeadlessWorldTest, ThrowingDigestProviderWithQueuedWorkLeavesNoStaleWork) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    ASSERT_NO_FATAL_FAILURE(ArmSchedulerFault(session, root, jobs, 3));
    const auto dt = session.GetSimulationClock().fixed_dt();
    ASSERT_EQ(session.TickSimulation(dt), 1u);
    ASSERT_EQ(session.TickSimulation(dt), 1u);
    ASSERT_EQ(session.TickSimulation(2.5 * dt), 1u);
    ASSERT_TRUE(session.GetRegionSchedulerFault().has_value());
    session.RecordActiveRegionWork({kFaultRegion, 5, 0, 0, 0});
    EXPECT_TRUE(session.IsSimulationTickBoundary());
    EXPECT_EQ(session.TickSimulation(dt), 0u);
    EXPECT_TRUE(session.IsSimulationTickBoundary());
    RemoveFaultRegionFile(session);
    EXPECT_TRUE(session.SaveWorldState());
}

TEST(GameSessionHeadlessWorldTest, CorruptLedgerLoadClearsTheWorld) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, victim;
    for (auto* s : {&owner, &victim}) {
        s->SetRootPath(root.root_string());
        s->SetJobSystem(&jobs);
        s->SetActiveRegionsEnabled(true);
    }
    ASSERT_TRUE(owner.CreateWorld("Corrupt ledger", "1337", "default"));
    ASSERT_EQ(owner.TickSimulation(owner.GetSimulationClock().fixed_dt()), 1u);
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    chunk->set_state(Luminumbra::ChunkState::Meshing);
    chunk->heightmap_data = {8.0f};
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(chunk));
    ASSERT_TRUE(owner.SaveWorldState());
    const auto save = owner.GetWorldSaveDir();
    const auto path = P::WorldSaveService::active_regions_path(save);
    auto bytes = ReadClockTestFile(path);
    bytes[8] ^= 1;
    std::ofstream(path, std::ios::binary) << bytes;
    ASSERT_TRUE(victim.CreateWorld("Victim", "2", "default"));
    auto resident = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    resident->set_state(Luminumbra::ChunkState::Meshing);
    resident->heightmap_data = {9.0f};
    resident->mark_voxel_data_dirty();
    ASSERT_TRUE(victim.GetWorldSystem()->adopt_streamed_chunk(resident));
    ASSERT_FALSE(victim.GetWorldSystem()->snapshot_streamed_chunks().empty());
    EXPECT_FALSE(victim.LoadWorldStateFrom(save));
    EXPECT_TRUE(victim.GetWorldSystem()->snapshot_streamed_chunks().empty());
    EXPECT_EQ(victim.GetWorldOpenError(), Luminumbra::world::ActiveRegionLedger::kCorruptMessage);
}

namespace {

void EnableSnapshotSession(GameSession& session, const HeadlessRoot& root, JobSystem& jobs) {
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    session.SetWaterHighResEnabled(false);
    session.SetWeatherRainEnabled(false);
}

} // namespace

TEST(GameSessionHeadlessWorldTest, SnapshotFlagOnSaveWritesIndexAndMarkerAndFlagOffDoesNot) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner;
    EnableSnapshotSession(owner, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Snapshot on", "1337", "default"));
    ASSERT_TRUE(owner.SaveWorldState());
    const auto save = owner.GetWorldSaveDir();
    EXPECT_TRUE(P::snapshot_index_exists(save));
    EXPECT_TRUE(fs::exists(save / "chunks/region/snapshot.json"));
    EXPECT_FALSE(P::journal_pending(save));
    const auto info = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    ASSERT_TRUE(info.contains("snapshotFormat"));
    EXPECT_EQ(info.at("snapshotFormat"), 1);
    P::SnapshotIndex index;
    ASSERT_TRUE(P::read_snapshot_index(save, index, nullptr));
    EXPECT_GE(index.generation, 1u);
    EXPECT_TRUE(std::any_of(
        index.members.begin(), index.members.end(), [](const P::SnapshotMember& member) {
            return member.path == "world_info.json" && member.present;
        }));

    const HeadlessRoot plain_root;
    GameSession plain;
    plain.SetActiveRegionsEnabled(false);
    plain.SetRootPath(plain_root.root_string());
    plain.SetJobSystem(&jobs);
    ASSERT_TRUE(plain.CreateWorld("Snapshot off", "1337", "default"));
    ASSERT_TRUE(plain.SaveWorldState());
    const auto plain_save = plain.GetWorldSaveDir();
    EXPECT_FALSE(P::snapshot_index_exists(plain_save));
    EXPECT_FALSE(fs::exists(P::journal_directory(plain_save)));
    const auto plain_info =
        nlohmann::json::parse(ReadClockTestFile(plain_save / "world_info.json"));
    EXPECT_FALSE(plain_info.contains("snapshotFormat"));
}

TEST(GameSessionHeadlessWorldTest, SnapshotReloadRoundTripsAndEachSaveBumpsGeneration) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, loaded;
    EnableSnapshotSession(owner, root, jobs);
    EnableSnapshotSession(loaded, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Snapshot reload", "1337", "default"));
    const auto save = owner.GetWorldSaveDir();
    ASSERT_TRUE(owner.SaveWorldState());
    P::SnapshotIndex first;
    ASSERT_TRUE(P::read_snapshot_index(save, first, nullptr));
    ASSERT_EQ(owner.TickSimulation(owner.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_TRUE(owner.SaveWorldState());
    P::SnapshotIndex second;
    ASSERT_TRUE(P::read_snapshot_index(save, second, nullptr));
    EXPECT_GT(second.generation, first.generation);
    EXPECT_TRUE(loaded.LoadWorld(owner.GetMetadata().worldId));
}

TEST(GameSessionHeadlessWorldTest, SnapshotMissingIndexWithMarkerIsRefused) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner;
    EnableSnapshotSession(owner, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Missing index", "1337", "default"));
    ASSERT_TRUE(owner.SaveWorldState());
    const auto save = owner.GetWorldSaveDir();
    const auto worldId = owner.GetMetadata().worldId;
    ASSERT_TRUE(fs::remove(P::snapshot_index_path(save)));
    GameSession reopened;
    EnableSnapshotSession(reopened, root, jobs);
    EXPECT_FALSE(reopened.LoadWorld(worldId));
    EXPECT_NE(reopened.GetWorldOpenError().find("inconsistent snapshot"), std::string::npos);
}

TEST(GameSessionHeadlessWorldTest, IndexedEnvelopeWitnessesSurviveMissingIndexAndMetadata) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner;
    EnableSnapshotSession(owner, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Envelope witness", "1337", "default"));
    ASSERT_TRUE(owner.SaveWorldState());
    const auto source = owner.GetWorldSaveDir();
    const auto manifest_path = P::WorldSaveService::world_manifest_path(source);
    ASSERT_TRUE(fs::exists(manifest_path)); // Required even with no chunks.
    const auto info = nlohmann::json::parse(ReadClockTestFile(source / "world_info.json"));
    const auto manifest = nlohmann::json::parse(ReadClockTestFile(manifest_path));
    EXPECT_EQ(info.at("container_version"), 3);
    EXPECT_EQ(manifest.at("schema"), "luminumbra.persistence.world_manifest.v2");
    for (int witness = 0; witness < 8; ++witness) {
        SCOPED_TRACE(witness);
        const auto save = root.path() / ("witness-" + std::to_string(witness));
        fs::copy(source, save, fs::copy_options::recursive);
        if (witness == 2) {
            ASSERT_TRUE(Luminumbra::Test::PublishIndexedSave(save, [&] {
                return P::WorldSaveService::remove_member(save, "world_info.json") &&
                       P::WorldSaveService::remove_member(save, "chunks/region/active-regions.arl");
            }));
            EXPECT_FALSE(fs::exists(save / "world_info.json"));
            EXPECT_TRUE(fs::exists(P::WorldSaveService::world_manifest_path(save)));
        }
        ASSERT_TRUE(fs::remove(P::snapshot_index_path(save)));
        if (witness == 0) {
            auto value = info;
            value.erase("snapshotFormat");
            std::ofstream(save / "world_info.json") << value.dump();
            ASSERT_TRUE(fs::remove(P::WorldSaveService::world_manifest_path(save)));
        } else if (witness == 1) {
            ASSERT_TRUE(fs::remove(save / "world_info.json"));
        } else if (witness == 3 || witness == 4) {
            const auto path = witness == 3 ? save / "world_info.json"
                                           : P::WorldSaveService::world_manifest_path(save);
            std::ofstream(path) << "{";
        } else if (witness == 5) {
            auto value = info;
            value["container_version"] = 4;
            std::ofstream(save / "world_info.json") << value.dump();
        } else if (witness == 6) {
            auto value = manifest;
            value["schema"] = "luminumbra.persistence.world_manifest.v3";
            std::ofstream(P::WorldSaveService::world_manifest_path(save)) << value.dump();
        } else if (witness == 7) {
            ASSERT_TRUE(fs::remove(P::WorldSaveService::world_manifest_path(save)));
            fs::create_directory(P::WorldSaveService::world_manifest_path(save));
        }
        const auto before_metadata = ReadClockTestFile(save / "world_info.json");
        std::vector<std::string> errors;
        EXPECT_FALSE(P::WorldSaveService::validate_save(save, &errors));
        EXPECT_FALSE(errors.empty());
        EXPECT_EQ(ReadClockTestFile(save / "world_info.json"), before_metadata);
        EXPECT_FALSE(P::journal_pending(save));
    }
}

TEST(GameSessionHeadlessWorldTest, SnapshotTamperedMemberIsRefused) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner;
    EnableSnapshotSession(owner, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Tampered member", "1337", "default"));
    ASSERT_TRUE(owner.SaveWorldState());
    const auto save = owner.GetWorldSaveDir();
    const auto worldId = owner.GetMetadata().worldId;
    // Rewrite valid metadata behind the index: the member keeps its format but not its fingerprint.
    auto info = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    info["name"] = "tampered behind the index";
    std::ofstream(save / "world_info.json", std::ios::binary | std::ios::trunc) << info.dump(2);
    GameSession refused;
    EnableSnapshotSession(refused, root, jobs);
    EXPECT_FALSE(refused.LoadWorld(worldId));
    EXPECT_NE(refused.GetWorldOpenError().find("inconsistent snapshot"), std::string::npos);
    ASSERT_TRUE(fs::remove(P::snapshot_index_path(save)));
    ASSERT_FALSE(fs::exists(P::journal_directory(save)));
    info.erase("snapshotFormat");
    {
        std::ofstream output(save / "world_info.json", std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output.is_open());
        output << info.dump(2) << "\n";
        output.close();
        ASSERT_FALSE(output.fail());
    }
    GameSession legacy;
    EnableSnapshotSession(legacy, root, jobs);
    (void)legacy.LoadWorld(worldId);
    EXPECT_NE(legacy.GetWorldOpenError().find("inconsistent snapshot"), std::string::npos);
}

TEST(GameSessionHeadlessWorldTest,
     SnapshotLegacyWorldWithoutIndexOrMarkerLoadsAndUpgradesOnFirstFlagOnSave) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner;
    owner.SetActiveRegionsEnabled(false);
    owner.SetRootPath(root.root_string());
    owner.SetJobSystem(&jobs);
    ASSERT_TRUE(owner.CreateWorld("Legacy upgrade", "1337", "default"));
    ASSERT_TRUE(owner.SaveWorldState());
    const auto save = owner.GetWorldSaveDir();
    const auto worldId = owner.GetMetadata().worldId;
    EXPECT_FALSE(P::snapshot_index_exists(save));
    GameSession legacy;
    EnableSnapshotSession(legacy, root, jobs);
    ASSERT_TRUE(legacy.LoadWorld(worldId));
    ASSERT_TRUE(legacy.SaveWorldState());
    EXPECT_TRUE(P::snapshot_index_exists(save));
    const auto info = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    ASSERT_TRUE(info.contains("snapshotFormat"));
    EXPECT_EQ(info.at("snapshotFormat"), 1);
}

TEST(GameSessionHeadlessWorldTest, SnapshotPendingJournalIsRolledBackAtOpen) {
    for (const bool torn_tail : {false, true}) {
        SCOPED_TRACE(torn_tail);
        const HeadlessRoot root;
        JobSystem jobs;
        jobs.startup(1);
        GameSession owner;
        EnableSnapshotSession(owner, root, jobs);
        ASSERT_TRUE(owner.CreateWorld("Pending journal", "1337", "default"));
        ASSERT_TRUE(owner.SaveWorldState());
        const auto save = owner.GetWorldSaveDir();
        const auto worldId = owner.GetMetadata().worldId;
        const auto info_bytes = ReadClockTestFile(save / "world_info.json");
        // Retain the actual interrupted journal objects: copied files have different ownership.
        const auto side = root.path() / "interrupted-journal";
        {
            P::SaveTransaction txn(save, 0, {}, &P::WorldSaveService::member_digest);
            ASSERT_TRUE(txn.begin(nullptr));
            ASSERT_TRUE(txn.protect({"world_info.json"}, nullptr));
            auto info = nlohmann::json::parse(info_bytes);
            info["name"] = "half written";
            ASSERT_TRUE(P::WorldSaveService::save_metadata(info.dump(2), save));
            fs::rename(P::journal_directory(save), side);
            txn.abort(nullptr); // Refuses the displaced journal and retains recovery evidence.
        }
        ASSERT_FALSE(P::journal_pending(save));
        fs::rename(side, P::journal_directory(save));
        if (torn_tail) {
            std::ofstream journal(P::journal_directory(save) / "txn.json",
                                  std::ios::binary | std::ios::app);
            journal << "{\"unpublished\":";
            journal.close();
            ASSERT_TRUE(journal.good());
        }
        const auto neutral = save / "0.tmp.1.2";
        {
            std::ofstream staging(neutral, std::ios::binary);
            staging << "unregistered partial staging";
            staging.close();
            ASSERT_TRUE(staging.good());
        }
        // Final empty-container retirement may leave either of these neutral
        // directory states after its control record has already been removed.
        const auto empty_retirement = save / ".world-rollback-interrupted";
        ASSERT_TRUE(fs::create_directories(empty_retirement / "artifact"));
        const auto entry = Luminumbra::Persistence::InspectSavedWorld(root.path(), worldId);
        EXPECT_TRUE(entry.pending_recovery);
        EXPECT_TRUE(entry.error.empty());
        EXPECT_TRUE(P::journal_pending(save));
        GameSession reopened;
        EnableSnapshotSession(reopened, root, jobs);
        ASSERT_TRUE(reopened.LoadWorld(worldId));
        EXPECT_FALSE(fs::exists(P::journal_directory(save)));
        EXPECT_EQ(ReadClockTestFile(save / "world_info.json"), info_bytes);
        EXPECT_EQ(ReadClockTestFile(neutral), "unregistered partial staging");
        EXPECT_TRUE(fs::is_empty(empty_retirement / "artifact"));
        ASSERT_TRUE(fs::remove(empty_retirement / "artifact"));
        ASSERT_TRUE(reopened.SaveWorldState());
        EXPECT_TRUE(fs::is_empty(empty_retirement));
        EXPECT_EQ(ReadClockTestFile(neutral), "unregistered partial staging");
    }
}

TEST(GameSessionHeadlessWorldTest, SnapshotSaveDecodesTheWorldAtMostOnce) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner;
    EnableSnapshotSession(owner, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Decode once", "1337", "default"));
    const auto dt = owner.GetSimulationClock().fixed_dt();
    for (int tick = 0; tick < 3; ++tick)
        ASSERT_EQ(owner.TickSimulation(dt), 1u);
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    chunk->set_state(Luminumbra::ChunkState::Meshing);
    chunk->heightmap_data = {8.0f};
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(chunk));
    ASSERT_TRUE(owner.SaveWorldState());
    chunk->mark_voxel_data_dirty(); // the warm-up save cleared the dirty flag
    const auto before = P::WorldSaveService::decode_count_for_testing();
    ASSERT_TRUE(owner.SaveWorldState());
    const auto after = P::WorldSaveService::decode_count_for_testing();
    EXPECT_LE(after - before, std::uint64_t{1});
}

TEST(GameSessionHeadlessWorldTest, UnchangedSnapshotSaveDoesNotReparseCommittedChunkRecords) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner;
    EnableSnapshotSession(owner, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Admitted records", "1337", "default"));
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    chunk->sdf_data.assign(std::size_t{17} * 17 * 17, -4.0f);
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(chunk));
    ASSERT_TRUE(owner.SaveWorldState());
    ASSERT_FALSE(chunk->is_voxel_data_dirty());
    // Validation still reads and digests every member file; only the payload
    // parse of record bytes already admitted is not repeated.
    const auto before = P::WorldSaveService::chunk_record_decode_count_for_testing();
    ASSERT_TRUE(owner.SaveWorldState());
    EXPECT_EQ(P::WorldSaveService::chunk_record_decode_count_for_testing(), before);
    const auto restored = P::WorldSaveService::read_chunk(owner.GetWorldSaveDir(), {0, 0, 0}, true);
    ASSERT_EQ(restored.status, P::WorldSaveService::ChunkReadStatus::Found);
    EXPECT_GT(P::WorldSaveService::chunk_record_decode_count_for_testing(), before);
}

TEST(GameSessionHeadlessWorldTest, RepeatedClientIdentityInspectionsReuseAdmittedChunkRecords) {
    using Service = P::WorldSaveService;
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner;
    EnableSnapshotSession(owner, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Repeated identity inspection", "94713", "default"));
    const auto plan =
        owner.GetWorldSystem()->GetInitialChunkLoadList(owner.GetMetadata().spawnPoint);
    ASSERT_GE(plan.size(), std::size_t{2});
    const Luminumbra::IVec3 excluded{plan.front().x + 64, plan.front().y, plan.front().z};
    // Three small, distinct records keep admission independent of earlier fixtures.
    for (const auto& coords : {plan[0], plan[1], excluded}) {
        auto chunk = std::make_shared<Luminumbra::Chunk>(coords);
        chunk->set_state(Luminumbra::ChunkState::Meshing);
        chunk->sdf_data.assign(std::size_t{17} * 17 * 17, -6.875f);
        chunk->heightmap_data = {613.125f, 614.375f};
        chunk->mark_voxel_data_dirty();
        ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(chunk));
    }
    ASSERT_TRUE(owner.SaveWorldState());
    const auto save = owner.GetWorldSaveDir();
    ASSERT_TRUE(P::snapshot_index_exists(save));
    const auto saved_files = ReadClockTestDirectory(save);
    GameSession reopened;
    EnableSnapshotSession(reopened, root, jobs);
    ASSERT_TRUE(reopened.LoadWorld(owner.GetMetadata().worldId)) << reopened.GetWorldOpenError();
    ASSERT_EQ(reopened.GetLastLoadedChunkCount(), std::size_t{2});
    const auto* admitted_world = reopened.GetWorldSystem();
    ASSERT_NE(admitted_world, nullptr);
    const auto admitted_tick = reopened.GetSimulationTickCount();
    const auto admitted_hash = SessionWorldHash(reopened);
    const auto expect_live_unchanged = [&]() {
        ASSERT_EQ(reopened.GetWorldSystem(), admitted_world);
        EXPECT_EQ(reopened.GetSimulationTickCount(), admitted_tick);
        EXPECT_TRUE(reopened.GetWorldOpenError().empty());
        EXPECT_FALSE(reopened.GetRegionSchedulerFault());
        EXPECT_EQ(admitted_world->snapshot_streamed_chunks().size(), std::size_t{2});
        EXPECT_EQ(admitted_world->find_streamed_chunk(excluded), nullptr);
        EXPECT_EQ(admitted_world->durable_chunk_inventory().size(), std::size_t{3});
        for (const auto& coords : {plan[0], plan[1]}) {
            EXPECT_NE(admitted_world->find_streamed_chunk(coords), nullptr);
        }
    };
    ASSERT_NO_FATAL_FAILURE(expect_live_unchanged());
    const auto expect_identity = [&]() {
        const auto before = Service::chunk_record_decode_count_for_testing();
        const auto identity = P::InspectSavedWorld(
            reopened.GetSaveRootPath(), root.path(), reopened.GetMetadata().worldId);
        ASSERT_TRUE(identity.error.empty()) << identity.error;
        EXPECT_FALSE(identity.pending_recovery);
        EXPECT_EQ(identity.metadata.worldId, owner.GetMetadata().worldId);
        EXPECT_EQ(identity.metadata.seed, owner.GetMetadata().seed);
        EXPECT_EQ(identity.metadata.worldType, owner.GetMetadata().worldType);
        EXPECT_EQ(identity.preset_path, save / "preset.json");
        EXPECT_EQ(Service::chunk_record_decode_count_for_testing(), before);
    };
    for (std::size_t inspection = 0; inspection < std::size_t{2}; ++inspection) {
        SCOPED_TRACE(inspection);
        ASSERT_NO_FATAL_FAILURE(expect_identity());
        ASSERT_NO_FATAL_FAILURE(expect_live_unchanged());
        EXPECT_EQ(SessionWorldHash(reopened), admitted_hash);
        EXPECT_EQ(ReadClockTestDirectory(save), saved_files);
    }

    int excluded_rx = 0;
    int excluded_rz = 0;
    Service::region_coords_for_chunk(excluded, excluded_rx, excluded_rz);
    const auto excluded_region = Service::region_file_path(save, excluded_rx, excluded_rz);
    const auto excluded_bytes = ReadClockTestFile(excluded_region);
    ASSERT_FALSE(excluded_bytes.empty());
    const auto parked_region = root.path() / "excluded-region.lmr";
    fs::rename(excluded_region, parked_region);
    const auto missing_files = ReadClockTestDirectory(save);
    const auto missing_decodes = Service::chunk_record_decode_count_for_testing();
    const auto missing_identity = P::InspectSavedWorld(
        reopened.GetSaveRootPath(), root.path(), reopened.GetMetadata().worldId);
    EXPECT_EQ(missing_identity.error,
              "inconsistent snapshot: " + Luminumbra::Filesystem::GenericUtf8PathString(
                                              excluded_region.lexically_relative(save)));
    EXPECT_EQ(Service::chunk_record_decode_count_for_testing(), missing_decodes);
    ASSERT_NO_FATAL_FAILURE(expect_live_unchanged());
    EXPECT_EQ(ReadClockTestDirectory(save), missing_files);
    EXPECT_EQ(ReadClockTestFile(parked_region), excluded_bytes);
    fs::rename(parked_region, excluded_region);
    ASSERT_NO_FATAL_FAILURE(expect_identity());
    ASSERT_NO_FATAL_FAILURE(expect_live_unchanged());
    // The authoritative hash reads durable records, so compare it after restoring the file.
    EXPECT_EQ(SessionWorldHash(reopened), admitted_hash);
    EXPECT_EQ(ReadClockTestDirectory(save), saved_files);
}

TEST(GameSessionHeadlessWorldTest, EncodedChunkRecordChecksMatchTheirDecode) {
    constexpr std::size_t kLattice = std::size_t{17} * 17 * 17;
    // Distinctive values keep these record bytes unseen by any earlier admission.
    const auto representative = []() {
        std::vector<std::shared_ptr<Luminumbra::Chunk>> chunks;
        const auto add = [&chunks](int x) {
            auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(x, 0, 5));
            chunk->heightmap_data = {7.75f, 8.25f};
            chunks.push_back(chunk);
            return chunk;
        };
        const auto generated = add(8);
        generated->sdf_data.assign(kLattice, 1.125f);
        generated->mark_sdf_generated_current_params();
        const auto edited = add(9);
        edited->sdf_data.assign(kLattice, -3.25f);
        edited->mark_voxel_data_dirty();
        const auto water_over_generated = add(10);
        water_over_generated->sdf_data.assign(kLattice, 1.375f);
        water_over_generated->has_water_sim.store(true);
        water_over_generated->set_water_authority();
        water_over_generated->mark_sdf_generated_current_params();
        const auto water_over_edited = add(11);
        water_over_edited->sdf_data.assign(kLattice, -1.625f);
        water_over_edited->has_water_sim.store(true);
        water_over_edited->mark_voxel_data_dirty();
        water_over_edited->set_water_authority();
        add(12)->mark_voxel_data_dirty(); // no SDF lattice
        const auto water_without_terrain = add(15);
        water_without_terrain->sdf_data.assign(kLattice, 1.875f);
        water_without_terrain->has_water_sim.store(true);
        water_without_terrain->set_water_authority();
        return chunks;
    };

    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner;
    EnableSnapshotSession(owner, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Encoded checks", "1337", "default"));
    for (const auto& chunk : representative())
        ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(chunk));
    // The commit digests every written region file in full; the records this
    // save encoded are admitted without a payload parse.
    const auto before = P::WorldSaveService::chunk_record_decode_count_for_testing();
    ASSERT_TRUE(owner.SaveWorldState());
    EXPECT_EQ(P::WorldSaveService::chunk_record_decode_count_for_testing(), before);
    P::WorldSaveService::AdmittedChunkRecordAudit audit;
    ASSERT_TRUE(P::WorldSaveService::audit_admitted_chunk_records_for_testing(
        P::WorldSaveService::region_file_path(owner.GetWorldSaveDir(), 0, 0), audit));
    EXPECT_GE(audit.records, std::size_t{6});
    EXPECT_EQ(audit.admitted, audit.records);
    EXPECT_EQ(audit.mismatched, std::size_t{0});
    // Water authority over non-authoritative terrain decodes as durable authority.
    for (const int x : {10, 15}) {
        SCOPED_TRACE(x);
        const auto decoded =
            P::WorldSaveService::read_chunk(owner.GetWorldSaveDir(), {x, 0, 5}, true);
        ASSERT_EQ(decoded.status, P::WorldSaveService::ChunkReadStatus::Found) << decoded.error;
        ASSERT_NE(decoded.chunk, nullptr);
        EXPECT_TRUE(decoded.chunk->has_water_authority());
        EXPECT_FALSE(decoded.chunk->has_terrain_authority());
        EXPECT_EQ(decoded.chunk->sdf_provenance(), Luminumbra::ChunkSdfProvenance::LoadedOrEdited);
    }
    // An inventory from admitted checks equals the one a full decode derives.
    const auto inventory_xs = [&owner](const std::unordered_set<Luminumbra::ChunkID>* retain) {
        Luminumbra::WorldStreamingState loaded;
        std::vector<std::string> load_errors;
        std::vector<Luminumbra::IVec3> inventory;
        EXPECT_TRUE(P::WorldSaveService{}.load_world(
            loaded, owner.GetWorldSaveDir(), load_errors, {}, retain, &inventory));
        EXPECT_TRUE(load_errors.empty());
        std::vector<int> xs;
        xs.reserve(inventory.size());
        for (const auto& coords : inventory)
            xs.push_back(coords.x);
        std::ranges::sort(xs);
        return xs;
    };
    const std::unordered_set<Luminumbra::ChunkID> retain_none;
    const auto admitted_inventory = inventory_xs(&retain_none);
    EXPECT_EQ(admitted_inventory, (std::vector<int>{9, 10, 11, 12, 15}));
    EXPECT_EQ(inventory_xs(nullptr), admitted_inventory);

    // A plain directory accepts records an indexed commit refuses: a partial
    // lattice is admitted as such, and a non-finite value is left to a real decode.
    Luminumbra::WorldStreamingState state;
    for (const auto& chunk : representative())
        ASSERT_TRUE(state.insert_chunk(chunk));
    const auto partial = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(13, 0, 5));
    partial->sdf_data = {-2.5f, 0.75f, 1.5f, 3.0f};
    partial->mark_voxel_data_dirty();
    ASSERT_TRUE(state.insert_chunk(partial));
    const auto non_finite = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(14, 0, 5));
    non_finite->sdf_data.assign(kLattice, 2.0f);
    non_finite->sdf_data[3] = std::numeric_limits<float>::quiet_NaN();
    non_finite->mark_voxel_data_dirty();
    ASSERT_TRUE(state.insert_chunk(non_finite));
    const fs::path plain = root.path() / "worlds" / "saves" / "encoded_checks_plain";
    std::vector<std::string> errors;
    ASSERT_TRUE(P::WorldSaveService{}.save_world(state, plain, &errors));
    ASSERT_TRUE(P::WorldSaveService::audit_admitted_chunk_records_for_testing(
        P::WorldSaveService::region_file_path(plain, 0, 0), audit));
    EXPECT_EQ(audit.records, std::size_t{8});
    EXPECT_EQ(audit.admitted, std::size_t{7});
    EXPECT_EQ(audit.mismatched, std::size_t{0});
}

TEST(GameSessionHeadlessWorldTest, SimulatedWaterIsPromotedBeforeSaveWhenRegionsAreOff) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner;
    owner.SetActiveRegionsEnabled(false);
    owner.SetRootPath(root.root_string());
    owner.SetJobSystem(&jobs);
    owner.SetWaterHighResEnabled(false);
    ASSERT_TRUE(owner.CreateWorld("Regions off water promotion", "1337", "default"));
    const Luminumbra::IVec3 coords(0, 0, 0);
    auto chunk = std::make_shared<Luminumbra::Chunk>(coords);
    chunk->set_state(Luminumbra::ChunkState::Idle);
    chunk->sdf_data.assign(std::size_t{17} * 17 * 17, -4.0f);
    chunk->mark_sdf_generated_current_params();
    chunk->current_water_resolution.store(8);
    chunk->water_depth_mm.assign(64, 77);
    chunk->water_bed_mm.assign(64, 0);
    chunk->water_edge_flux.assign(128, 0);
    chunk->water_level_data.assign(64, 0.077f);
    chunk->water_flow_data.assign(64, Luminumbra::Vec2(0.0f));
    chunk->water_sim_terrain_height.assign(64, 0.0f);
    chunk->water_rest_level.assign(64, 0.077f);
    chunk->has_water_sim.store(true);
    ASSERT_TRUE(chunk->has_complete_water_state());
    ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(chunk));
    // A simulation writer marks the chunk for saving without claiming ownership itself.
    chunk->mark_dirty_for_save();
    chunk->mark_water_state_dirty();
    ASSERT_FALSE(chunk->has_water_authority());
    ASSERT_TRUE(owner.SaveWorldState());
    EXPECT_TRUE(chunk->has_water_authority());
    const auto saved = P::WorldSaveService::read_chunk(owner.GetWorldSaveDir(), coords);
    ASSERT_EQ(saved.status, P::WorldSaveService::ChunkReadStatus::Found);
    ASSERT_NE(saved.chunk, nullptr);
    EXPECT_TRUE(saved.chunk->has_water_authority());
    EXPECT_TRUE(saved.chunk->same_water_state(*chunk));
}

namespace {

std::shared_ptr<Luminumbra::Chunk> AdoptUnpromotedWaterChunk(GameSession& session,
                                                             bool changed = true) {
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    chunk->set_state(Luminumbra::ChunkState::Idle);
    chunk->sdf_data.assign(std::size_t{17} * 17 * 17, -4.0f);
    chunk->mark_sdf_generated_current_params();
    chunk->current_water_resolution.store(8);
    chunk->water_depth_mm.assign(64, 77);
    chunk->water_bed_mm.assign(64, 0);
    chunk->water_edge_flux.assign(128, 0);
    chunk->water_level_data.assign(64, 0.077f);
    chunk->water_flow_data.assign(64, Luminumbra::Vec2(0.0f));
    chunk->water_sim_terrain_height.assign(64, 0.0f);
    chunk->water_rest_level.assign(64, 0.077f);
    chunk->has_water_sim.store(true);
    EXPECT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
    if (changed) {
        chunk->mark_dirty_for_save();
        chunk->mark_water_state_dirty();
    }
    return chunk;
}

void CreateRegionsOffWaterSession(GameSession& session, const HeadlessRoot& root, JobSystem& jobs) {
    session.SetActiveRegionsEnabled(false);
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetWaterHighResEnabled(false);
    ASSERT_TRUE(session.CreateWorld("Regions off hash timing", "1337", "default"));
}

void MutateWaterOnTick(GameSession& session, const std::shared_ptr<Luminumbra::Chunk>& chunk) {
    session.GetSimulationEventBus().subscribe([chunk](const auto&) {
        EXPECT_FALSE(chunk->has_water_authority());
        ++chunk->water_depth_mm.front();
        chunk->mark_water_state_dirty();
    });
    session.GetSimulationEventBus().publish(1, "water.change", "");
}

void ExpectWaterHashReadsLeavePendingChange(GameSession& session,
                                            const std::shared_ptr<Luminumbra::Chunk>& chunk) {
    const auto inputs = session.GetWorldSystem()->water_authority_input_items();
    const auto hash = session.ComputeChunkStateHash();
    EXPECT_EQ(session.ComputeChunkStateHash(), hash);
    EXPECT_FALSE(chunk->has_water_authority());
    EXPECT_TRUE(chunk->is_water_state_dirty());
    EXPECT_EQ(session.GetWorldSystem()->water_authority_input_items(), inputs);
}

} // namespace

TEST(GameSessionHeadlessWorldTest, WaterOwnershipHashDoesNotDependOnSaveTimingWhenRegionsAreOff) {
    const HeadlessRoot root_a;
    const HeadlessRoot root_b;
    JobSystem jobs;
    jobs.startup(1);
    GameSession saved;
    GameSession unsaved;
    ASSERT_NO_FATAL_FAILURE(CreateRegionsOffWaterSession(saved, root_a, jobs));
    ASSERT_NO_FATAL_FAILURE(CreateRegionsOffWaterSession(unsaved, root_b, jobs));
    const auto saved_chunk = AdoptUnpromotedWaterChunk(saved, false);
    const auto unsaved_chunk = AdoptUnpromotedWaterChunk(unsaved, false);
    MutateWaterOnTick(saved, saved_chunk);
    MutateWaterOnTick(unsaved, unsaved_chunk);
    ASSERT_EQ(saved.TickSimulation(saved.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_EQ(unsaved.TickSimulation(unsaved.GetSimulationClock().fixed_dt()), 1u);
    // Read the raw world projection first: no hash API may repair the tick boundary.
    const auto world_before_save = SessionWorldHash(saved);
    const auto before_save = saved.ComputeChunkStateHash();
    const auto inputs = saved.GetWorldSystem()->water_authority_input_items();
    ASSERT_TRUE(saved.SaveWorldState());
    EXPECT_EQ(saved.ComputeChunkStateHash(), before_save);
    EXPECT_EQ(unsaved.ComputeChunkStateHash(), before_save);
    EXPECT_EQ(SessionWorldHash(saved), world_before_save);
    EXPECT_EQ(SessionWorldHash(unsaved), world_before_save);
    EXPECT_EQ(saved.GetWorldSystem()->water_authority_input_items(), inputs);
    EXPECT_TRUE(saved_chunk->has_water_authority());
    EXPECT_TRUE(unsaved_chunk->has_water_authority());
}

TEST(GameSessionHeadlessWorldTest, WaterOwnershipIsPromotedAtTheTickBoundaryWhenRegionsAreOff) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    ASSERT_NO_FATAL_FAILURE(CreateRegionsOffWaterSession(session, root, jobs));
    const auto chunk = AdoptUnpromotedWaterChunk(session, false);
    MutateWaterOnTick(session, chunk);
    ASSERT_FALSE(chunk->has_water_authority());
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    EXPECT_TRUE(chunk->has_water_authority());
}

TEST(GameSessionHeadlessWorldTest, WaterHashReadsDoNotPromoteChangesOutsideTicks) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    ASSERT_NO_FATAL_FAILURE(CreateRegionsOffWaterSession(session, root, jobs));
    const auto chunk = AdoptUnpromotedWaterChunk(session);
    ASSERT_EQ(session.TickSimulation(0.0), 0u);
    ExpectWaterHashReadsLeavePendingChange(session, chunk);
    // Persistence also supports edits made outside a tick and must own the saved water.
    ASSERT_TRUE(session.SaveWorldState());
    EXPECT_TRUE(chunk->has_water_authority());
}

TEST(GameSessionHeadlessWorldTest,
     TickBoundaryWaterPromotionVisitsOnlyChangedChunksWhenRegionsAreOff) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    ASSERT_NO_FATAL_FAILURE(CreateRegionsOffWaterSession(session, root, jobs));
    const auto chunk = AdoptUnpromotedWaterChunk(session);
    // Clean resident chunks must not enter the authority queue on any tick.
    for (int x = 1; x <= 24; ++x) {
        auto clean = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(x, 0, 0));
        clean->set_state(Luminumbra::ChunkState::Idle);
        clean->mark_sdf_generated_current_params();
        ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(clean));
    }
    const auto fixed_dt = session.GetSimulationClock().fixed_dt();
    const auto inputs_before = session.GetWorldSystem()->water_authority_input_items();
    ASSERT_EQ(session.TickSimulation(fixed_dt), 1u);
    const auto inputs_after_change = session.GetWorldSystem()->water_authority_input_items();
    EXPECT_EQ(inputs_after_change - inputs_before, 1u);
    // Nothing changed: later ticks must not revisit the resident chunk at all.
    ASSERT_EQ(session.TickSimulation(fixed_dt), 1u);
    ASSERT_EQ(session.TickSimulation(fixed_dt), 1u);
    EXPECT_EQ(session.GetWorldSystem()->water_authority_input_items(), inputs_after_change);
    chunk->mark_water_state_dirty();
    chunk->mark_water_state_dirty();
    ASSERT_EQ(session.TickSimulation(fixed_dt), 1u);
    EXPECT_EQ(session.GetWorldSystem()->water_authority_input_items() - inputs_after_change, 1u);
}

TEST(GameSessionHeadlessWorldTest, WaterOnlySaveWithRegionsOffKeepsDigestThroughRegionsUpgrade) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner;
    owner.SetActiveRegionsEnabled(false);
    owner.SetRootPath(root.root_string());
    owner.SetJobSystem(&jobs);
    owner.SetWaterHighResEnabled(false);
    ASSERT_TRUE(owner.CreateWorld("Water-only regions upgrade", "1337", "default"));
    const Luminumbra::IVec3 distant(1280, 0, -1280);
    const Luminumbra::world::RegionKey key{40, -40};
    auto chunk = std::make_shared<Luminumbra::Chunk>(distant);
    chunk->set_state(Luminumbra::ChunkState::Idle);
    chunk->sdf_data.assign(std::size_t{17} * 17 * 17, -4.0f);
    chunk->mark_sdf_generated_current_params();
    chunk->current_water_resolution.store(8);
    chunk->water_depth_mm.assign(64, 123);
    chunk->water_bed_mm.assign(64, 0);
    chunk->water_edge_flux.assign(128, 0);
    chunk->water_level_data.assign(64, 0.123f);
    chunk->water_flow_data.assign(64, Luminumbra::Vec2(0.0f));
    chunk->water_sim_terrain_height.assign(64, 0.0f);
    chunk->water_rest_level.assign(64, 0.123f);
    chunk->has_water_sim.store(true);
    chunk->is_water_sleeping.store(true);
    chunk->mark_water_data_dirty();
    ASSERT_TRUE(chunk->has_complete_water_state());
    ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(chunk));
    ASSERT_EQ(chunk->sdf_provenance(), Luminumbra::ChunkSdfProvenance::GeneratedCurrentParams);
    ASSERT_FALSE(chunk->has_terrain_authority());
    const auto digest = Digest({}, key, {chunk});
    ASSERT_NE(digest, Digest({}, key, {}));
    ASSERT_TRUE(owner.SaveWorldState());
    const auto save = owner.GetWorldSaveDir();
    const auto world_id = owner.GetMetadata().worldId;
    ASSERT_FALSE(P::snapshot_index_exists(save));
    EXPECT_EQ(chunk->sdf_provenance(), Luminumbra::ChunkSdfProvenance::GeneratedCurrentParams);
    EXPECT_FALSE(chunk->has_terrain_authority());
    EXPECT_EQ(Digest(save, key, {chunk}), digest);
    std::vector<P::WorldSaveService::ContainerRecord> records;
    ASSERT_TRUE(P::WorldSaveService::read_container_records(
        P::WorldSaveService::region_file_path(save, key.x, key.z), records));
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records.front().id, chunk->get_id());
    EXPECT_NE(records.front().flags & 0x01u, 0u);
    const auto saved = P::WorldSaveService::read_chunk(save, distant);
    ASSERT_EQ(saved.status, P::WorldSaveService::ChunkReadStatus::Found);
    ASSERT_NE(saved.chunk, nullptr);
    EXPECT_TRUE(saved.chunk->same_water_state(*chunk));
    EXPECT_TRUE(saved.chunk->has_water_authority());
    EXPECT_FALSE(saved.chunk->has_terrain_authority());

    const auto expect_digests = [&](GameSession& session) {
        EXPECT_EQ(Digest(save, key, {}), digest);
        auto* world = session.GetWorldSystem();
        ASSERT_TRUE(world->restore_authoritative_chunk(distant));
        const auto restored = world->find_streamed_chunk(distant);
        ASSERT_NE(restored, nullptr);
        EXPECT_TRUE(restored->same_water_state(*chunk));
        EXPECT_TRUE(restored->has_water_authority());
        EXPECT_FALSE(restored->has_terrain_authority());
        EXPECT_EQ(Digest(save, key, {restored}), digest);
        EXPECT_EQ(Digest(save, key, {}, {restored}), digest);
        auto inputs = world->region_digest_inputs(key.x, key.z);
        inputs.require_index = P::snapshot_index_exists(save);
        EXPECT_EQ(P::WorldSaveService::region_simulation_digest(save, key, inputs), digest);
    };
    GameSession upgraded;
    EnableSnapshotSession(upgraded, root, jobs);
    ASSERT_TRUE(upgraded.LoadWorld(world_id)) << upgraded.GetWorldOpenError();
    ASSERT_FALSE(upgraded.GetWorldSystem()->find_streamed_chunk(distant));
    ASSERT_NO_FATAL_FAILURE(expect_digests(upgraded));
    ASSERT_TRUE(upgraded.SaveWorldState());
    ASSERT_TRUE(P::snapshot_index_exists(save));
    ASSERT_NO_FATAL_FAILURE(expect_digests(upgraded));
    GameSession reopened;
    EnableSnapshotSession(reopened, root, jobs);
    ASSERT_TRUE(reopened.LoadWorld(world_id)) << reopened.GetWorldOpenError();
    ASSERT_FALSE(reopened.GetWorldSystem()->find_streamed_chunk(distant));
    ASSERT_NO_FATAL_FAILURE(expect_digests(reopened));
}

TEST(GameSessionHeadlessWorldTest, LegacyEvolvedWaterOnGeneratedChunkSurvivesRegionsUpgrade) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner;
    owner.SetActiveRegionsEnabled(false);
    owner.SetRootPath(root.root_string());
    owner.SetJobSystem(&jobs);
    owner.SetWaterHighResEnabled(false);
    ASSERT_TRUE(owner.CreateWorld("Legacy water", "1337", "default"));
    const Luminumbra::IVec3 distant(1280, 0, -1280);
    auto chunk = std::make_shared<Luminumbra::Chunk>(distant);
    chunk->sdf_data.assign(std::size_t{17} * 17 * 17, -4.0f);
    chunk->mark_sdf_generated_current_params();
    chunk->water_depth_mm.assign(64, 123);
    chunk->water_bed_mm.assign(64, 0);
    chunk->water_edge_flux.assign(128, 0);
    chunk->has_water_sim.store(true);
    // No water-state mark: this fixture is the old record shape, water without its flag.
    chunk->mark_dirty_for_save();
    ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(chunk));
    ASSERT_TRUE(owner.SaveWorldState());
    const auto save = owner.GetWorldSaveDir();
    const auto worldId = owner.GetMetadata().worldId;
    ASSERT_FALSE(P::snapshot_index_exists(save));
    const auto evolved = chunk->water_depth_mm;
    {
        // The legacy record carries the water but not the edited flag.
        Luminumbra::WorldStreamingState state;
        std::vector<std::string> errors;
        ASSERT_TRUE(P::WorldSaveService{}.load_world(state, save, errors));
        const auto legacy = state.find_chunk(distant);
        ASSERT_NE(legacy, nullptr);
        EXPECT_EQ(legacy->sdf_provenance(), Luminumbra::ChunkSdfProvenance::GeneratedCurrentParams);
        EXPECT_FALSE(legacy->has_water_authority());
        EXPECT_EQ(legacy->water_depth_mm, evolved);
    }
    GameSession upgraded;
    EnableSnapshotSession(upgraded, root, jobs);
    ASSERT_TRUE(upgraded.LoadWorld(worldId));
    const auto adopted = upgraded.GetWorldSystem()->find_streamed_chunk(distant);
    ASSERT_NE(adopted, nullptr);
    EXPECT_TRUE(adopted->has_water_authority());
    EXPECT_EQ(adopted->water_depth_mm, evolved);
    ASSERT_TRUE(upgraded.SaveWorldState());
    ASSERT_TRUE(P::snapshot_index_exists(save));
    const auto indexed = P::WorldSaveService::read_chunk(save, distant, true);
    ASSERT_EQ(indexed.status, P::WorldSaveService::ChunkReadStatus::Found);
    ASSERT_NE(indexed.chunk, nullptr);
    EXPECT_TRUE(indexed.chunk->has_water_authority());
    EXPECT_EQ(indexed.chunk->water_depth_mm, evolved);
    GameSession reopened;
    EnableSnapshotSession(reopened, root, jobs);
    ASSERT_TRUE(reopened.LoadWorld(worldId));
    ASSERT_TRUE(reopened.GetWorldSystem()->restore_authoritative_chunk(distant));
    const auto restored = reopened.GetWorldSystem()->find_streamed_chunk(distant);
    ASSERT_NE(restored, nullptr);
    EXPECT_TRUE(restored->has_water_authority());
    EXPECT_EQ(restored->water_depth_mm, evolved);
}

TEST(GameSessionHeadlessWorldTest, BoundedBootSaveAsTransfersDiskOnlyAuthorityAndFaultsOnLoss) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, loaded, copied;
    for (auto* session : {&owner, &loaded, &copied})
        EnableSnapshotSession(*session, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Disk authority", "1337", "default"));
    const Luminumbra::IVec3 distant(1280, 0, -1280);
    auto chunk = std::make_shared<Luminumbra::Chunk>(distant);
    chunk->sdf_data.assign(17 * 17 * 17, -4.0f);
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(chunk));
    const auto before = owner.ComputeChunkStateHash();
    ASSERT_TRUE(owner.SaveWorldState());
    ASSERT_TRUE(loaded.LoadWorld(owner.GetMetadata().worldId));
    EXPECT_FALSE(loaded.GetWorldSystem()->find_streamed_chunk(distant));
    EXPECT_EQ(loaded.GetWorldSystem()->durable_chunk_inventory().size(), 1u);
    EXPECT_EQ(loaded.ComputeChunkStateHash(), before);
    const auto destination = root.path() / "disk-authority-copy";
    ASSERT_TRUE(loaded.SaveWorldStateTo(destination));
    // A named save copies exported disk-only authority back to the canonical world.
    ASSERT_TRUE(loaded.SaveWorld());
    EXPECT_EQ(loaded.ComputeChunkStateHash(), before);
    ASSERT_TRUE(copied.CreateTransientWorld("Copy", "1337", "default"));
    ASSERT_TRUE(copied.LoadWorldStateFrom(destination));
    EXPECT_FALSE(copied.GetWorldSystem()->find_streamed_chunk(distant));
    EXPECT_EQ(copied.ComputeChunkStateHash(), before);
    ASSERT_TRUE(copied.GetWorldSystem()->restore_authoritative_chunk(distant));
    const auto restored = copied.GetWorldSystem()->find_streamed_chunk(distant);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->sdf_data, chunk->sdf_data);
    const auto source_region = P::WorldSaveService::region_file_path(destination, 40, -40);
    ASSERT_TRUE(fs::remove(source_region));
    // A fresh bounded boot still refuses the lost distant member before adopting any chunk.
    GameSession refused;
    EnableSnapshotSession(refused, root, jobs);
    ASSERT_TRUE(refused.CreateTransientWorld("Refused", "1337", "default"));
    EXPECT_FALSE(refused.LoadWorldStateFrom(destination));
    EXPECT_TRUE(refused.GetWorldSystem()->snapshot_streamed_chunks().empty());
    // The named save retargeted loaded to its canonical directory. Losing that
    // independent copy must also latch a fault rather than generate replacements.
    const auto canonical = root.path() / "worlds/saves" / loaded.GetMetadata().worldId;
    ASSERT_TRUE(fs::remove(P::WorldSaveService::region_file_path(canonical, 40, -40)));
    EXPECT_TRUE(loaded.GetWorldSystem()->restore_authoritative_chunk(distant));
    EXPECT_TRUE(loaded.GetRegionSchedulerFault().has_value());
    EXPECT_FALSE(loaded.GetWorldSystem()->find_streamed_chunk(distant));
    EXPECT_EQ(loaded.GetWorldSystem()->EditTerrainVoxel({20488, 8, -20472}, 1, true, nullptr), 0);
    EXPECT_EQ(loaded.TickSimulation(loaded.GetSimulationClock().fixed_dt()), 0u);
}

TEST(GameSessionHeadlessWorldTest, ValidatedStateAdoptionClearsTheDurableReadLatch) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, loaded;
    EnableSnapshotSession(owner, root, jobs);
    EnableSnapshotSession(loaded, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Durable latch", "1337", "default"));
    const Luminumbra::IVec3 distant(1280, 0, -1280);
    auto chunk = std::make_shared<Luminumbra::Chunk>(distant);
    chunk->sdf_data.assign(17 * 17 * 17, -4.0f);
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(chunk));
    ASSERT_TRUE(owner.SaveWorldState());
    ASSERT_TRUE(loaded.LoadWorld(owner.GetMetadata().worldId));
    const auto directory = root.path() / "worlds/saves" / owner.GetMetadata().worldId;
    const auto region = P::WorldSaveService::region_file_path(directory, 40, -40);
    const auto aside = root.path() / "durable-latch-region";
    std::error_code moved;
    fs::rename(region, aside, moved);
    ASSERT_FALSE(moved);
    // A lost durable member latches both the session fault and the world's read latch.
    EXPECT_TRUE(loaded.GetWorldSystem()->restore_authoritative_chunk(distant));
    ASSERT_TRUE(loaded.GetRegionSchedulerFault().has_value());
    EXPECT_FALSE(loaded.GetWorldSystem()->find_streamed_chunk(distant));
    fs::rename(aside, region, moved);
    ASSERT_FALSE(moved);
    // Adopting validated saved state clears both; durable reads serve the member again.
    ASSERT_TRUE(loaded.LoadWorldStateFrom(directory));
    EXPECT_FALSE(loaded.GetRegionSchedulerFault().has_value());
    ASSERT_TRUE(loaded.GetWorldSystem()->restore_authoritative_chunk(distant));
    const auto restored = loaded.GetWorldSystem()->find_streamed_chunk(distant);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->sdf_data, chunk->sdf_data);
    EXPECT_FALSE(loaded.GetRegionSchedulerFault().has_value());
}

namespace {
Luminumbra::Chunk*& ChunkRevisionDuringCommit() {
    static Luminumbra::Chunk* chunk = nullptr;
    return chunk;
}
void ChangeChunkAtIndexPublication(const char* step, const fs::path& path) {
    if (ChunkRevisionDuringCommit() && std::string(step) == "before-replace" &&
        path.filename() == "snapshot.json") {
        ChunkRevisionDuringCommit()->sdf_data.front() -= 1.0f;
        ChunkRevisionDuringCommit()->mark_voxel_data_dirty();
        ChunkRevisionDuringCommit()->mark_water_state_dirty();
    }
}
struct ChunkRevisionHook {
    explicit ChunkRevisionHook(Luminumbra::Chunk* chunk) {
        ChunkRevisionDuringCommit() = chunk;
        P::set_snapshot_step_hook_for_testing(&ChangeChunkAtIndexPublication);
    }
    ~ChunkRevisionHook() {
        P::set_snapshot_step_hook_for_testing(nullptr);
        ChunkRevisionDuringCommit() = nullptr;
    }
};
} // namespace

TEST(GameSessionHeadlessWorldTest, FirstAndIncrementalSavePreserveChangesAfterSerialization) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner;
    EnableSnapshotSession(owner, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Revision acknowledgement", "1337", "default"));
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    chunk->sdf_data.assign(17 * 17 * 17, -1.0f);
    chunk->mark_voxel_data_dirty();
    chunk->mark_water_state_dirty();
    ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(chunk));
    for (const bool first : {true, false}) {
        SCOPED_TRACE(first);
        const auto serialized_sdf = chunk->sdf_data;
        {
            ChunkRevisionHook hook(chunk.get());
            ASSERT_TRUE(owner.SaveWorldState());
        }
        EXPECT_TRUE(chunk->is_voxel_data_dirty());
        EXPECT_TRUE(chunk->is_water_state_dirty());
        const auto durable =
            P::WorldSaveService::read_chunk(owner.GetWorldSaveDir(), {0, 0, 0}, true);
        ASSERT_EQ(durable.status, P::WorldSaveService::ChunkReadStatus::Found);
        ASSERT_NE(durable.chunk, nullptr);
        EXPECT_EQ(durable.chunk->sdf_data, serialized_sdf);
        EXPECT_NE(chunk->sdf_data, serialized_sdf);
    }
    ASSERT_TRUE(owner.SaveWorldState());
    EXPECT_FALSE(chunk->is_voxel_data_dirty());
    EXPECT_FALSE(chunk->is_water_state_dirty());
}

namespace {
using Luminumbra::Chunk;
using Luminumbra::ChunkID;
using Luminumbra::ChunkSdfProvenance;
using Luminumbra::world::ActiveRegionLedger;
using Luminumbra::world::ActiveRegionRecord;
using Luminumbra::world::RegionKey;
using Luminumbra::world::RegionState;
struct FrozenWaterSnapshot {
    std::vector<std::int32_t> depth, bed, flux;
};

struct RegionalWaterScenario {
    // WaterSystem retains this map's address; destroy the session before the map.
    std::unordered_map<ChunkID, std::shared_ptr<Chunk>> water_chunks;
    GameSession session;
};

void SeedRegionalWaterFixture(RegionalWaterScenario& scenario,
                              const std::shared_ptr<Luminumbra::Chunk>& chunk,
                              RegionKey water_region,
                              RegionKey work_region,
                              const Luminumbra::Vec3& visit_position,
                              std::uint64_t empty_digest,
                              std::vector<std::int32_t>& generated_depth) {
    auto& session = scenario.session;
    scenario.water_chunks.emplace(chunk->get_id(), chunk);
    const auto before_initialization = P::WorldSaveService::region_simulation_digest(
        {}, water_region, P::RegionDigestInputs{{chunk}, {}, {}});
    session.GetWaterSystem()->update(session.GetRegistry(), scenario.water_chunks);
    ASSERT_TRUE(chunk->has_water_sim.load());
    ASSERT_EQ(chunk->water_depth_mm.size(), 64u);
    ASSERT_EQ(chunk->water_bed_mm.size(), 64u);
    ASSERT_EQ(chunk->water_edge_flux.size(), 128u);
    ASSERT_EQ(chunk->sdf_provenance(), ChunkSdfProvenance::GeneratedCurrentParams);
    ASSERT_FALSE(chunk->is_water_state_dirty());
    ASSERT_FALSE(chunk->has_water_authority());
    ASSERT_FALSE(chunk->is_voxel_data_dirty());
    generated_depth = chunk->water_depth_mm;
    EXPECT_EQ(before_initialization, empty_digest);
    EXPECT_EQ(P::WorldSaveService::region_simulation_digest(
                  {}, water_region, P::RegionDigestInputs{{chunk}, {}, {}}),
              before_initialization);

    session.PinActiveRegion(work_region, true);
    session.SetReplicatedSimulationAnchors({visit_position});
    const auto dt = session.GetSimulationClock().fixed_dt();
    ASSERT_EQ(session.TickSimulation(dt), 1u);
    const auto initial_record = session.GetActiveRegionLedger().records().at(water_region);
    ASSERT_TRUE(initial_record.visited);
    ASSERT_FALSE(initial_record.pinned);
    ASSERT_FALSE(initial_record.edited);
    ASSERT_FALSE(initial_record.populated);
    ASSERT_EQ(initial_record.water_cursor, 0u);
    ASSERT_EQ(initial_record.state, RegionState::Active);
    ASSERT_FALSE(chunk->has_water_authority());
}

void FreezeRegionalWaterFixture(RegionalWaterScenario& scenario,
                                const std::shared_ptr<Luminumbra::Chunk>& chunk,
                                bool displace,
                                RegionKey water_region,
                                RegionKey work_region,
                                const Luminumbra::Vec3& displacement_position,
                                const std::vector<std::int32_t>& generated_depth,
                                const ActiveRegionRecord& initial_record) {
    auto& session = scenario.session;
    const auto dt = session.GetSimulationClock().fixed_dt();
    if (displace) {
        session.GetWaterSystem()->apply_displacement(displacement_position, 4.0f);
        ASSERT_NE(chunk->water_depth_mm, generated_depth);
        ASSERT_TRUE(chunk->is_water_state_dirty());
        // The writer itself must leave promotion to the scheduling boundary under test.
        ASSERT_EQ(chunk->sdf_provenance(), ChunkSdfProvenance::GeneratedCurrentParams);
        ASSERT_FALSE(chunk->has_water_authority());
    }
    // This fixture drives WaterSystem directly, outside TickSimulation. Complete that writer's
    // boundary before asking the scheduler to freeze or retain the resulting ownership.
    session.GetWorldSystem()->sync_water_authority();
    session.SetReplicatedSimulationAnchors({});
    session.RecordActiveRegionWork({work_region, 1, 0, 0, 0});
    ASSERT_EQ(session.TickSimulation(dt), 1u);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_EQ(chunk->has_water_authority(), displace);
    EXPECT_EQ(chunk->sdf_provenance(),
              displace ? ChunkSdfProvenance::LoadedOrEdited
                       : ChunkSdfProvenance::GeneratedCurrentParams);
    EXPECT_EQ(chunk->is_voxel_data_dirty(), displace);
    EXPECT_FALSE(chunk->is_water_state_dirty());
    // K is still unfrozen when the first post-writer boundary has promoted its water.
    ASSERT_NE(session.GetActiveRegionLedger().records().at(water_region).state,
              RegionState::Frozen);

    // At most two budget transitions per unpinned record suffice; only J supplies work.
    const auto transition_count = 2 * session.GetActiveRegionLedger().records().size();
    for (std::size_t step = 0; step < transition_count; ++step) {
        session.RecordActiveRegionWork({work_region, 1, 0, 0, 0});
        ASSERT_EQ(session.TickSimulation(dt), 1u);
        ASSERT_FALSE(session.GetRegionSchedulerFault());
    }
    const auto frozen_record = session.GetActiveRegionLedger().records().at(water_region);
    ASSERT_EQ(frozen_record.state, RegionState::Frozen);
    EXPECT_FALSE(frozen_record.pinned);
    EXPECT_FALSE(frozen_record.edited);
    EXPECT_FALSE(frozen_record.populated);
    EXPECT_EQ(frozen_record.water_cursor, 0u);
    EXPECT_EQ(frozen_record.pressure, 0u);
    EXPECT_EQ(frozen_record.last_proximity, initial_record.last_proximity);
    EXPECT_EQ(frozen_record.frozen_digest,
              P::WorldSaveService::region_simulation_digest(
                  {}, water_region, P::RegionDigestInputs{{chunk}, {}, {}}));
    const auto* world = session.GetWorldSystem();
    const auto before_projection = world->region_digest_input_items();
    for (const auto& [rx, rz] : world->authority_region_keys()) {
        const auto record = session.GetActiveRegionLedger().records().find({rx, rz});
        if (record == session.GetActiveRegionLedger().records().end() ||
            record->second.state != RegionState::Frozen)
            (void)world->region_digest_inputs(rx, rz);
    }
    const auto active_inputs = world->region_digest_input_items() - before_projection;
    const auto before_hash = world->region_digest_input_items();
    const auto hash = session.ComputeChunkStateHash();
    EXPECT_EQ(world->region_digest_input_items() - before_hash, active_inputs);
    EXPECT_EQ(session.ComputeChunkStateHash(), hash);
}

void PersistRegionalWaterBeforeRetention(GameSession& session,
                                         const std::shared_ptr<Chunk>& chunk,
                                         RegionKey work_region,
                                         const Luminumbra::IVec3& coords,
                                         const ActiveRegionRecord& frozen_record) {
    using Luminumbra::Vec3;
    using Luminumbra::world::RegionWork;
    using Luminumbra::world::WorldClock;
    ASSERT_TRUE(session.SaveWorldState());
    EXPECT_FALSE(chunk->is_voxel_data_dirty());
    EXPECT_TRUE(chunk->has_water_authority());

    // Use the existing paired-clock/ledger fixture idiom to approach retention directly.
    auto staged = session.GetActiveRegionLedger();
    const WorldClock before_idle(frozen_record.last_proximity +
                                 ActiveRegionLedger::kIdleEvictionTicks - 1);
    const std::array pressure{RegionWork{work_region, 1, 0, 0, 0}};
    staged.schedule(before_idle, std::span<const Vec3>{}, pressure);
    const auto save = session.GetWorldSaveDir();
    auto metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    before_idle.write_metadata(metadata);
    ASSERT_TRUE(
        P::WorldSaveService::save_metadata_and_active_regions(metadata.dump(), staged, save));
    ASSERT_TRUE(session.LoadWorldStateFrom(save)) << session.GetWorldOpenError();
    ASSERT_EQ(session.GetWorldSystem()->find_streamed_chunk(coords), chunk);
}

void ReopenRegionalWaterBeforeRetention(GameSession& session,
                                        GameSession& reopened,
                                        const HeadlessRoot& root,
                                        JobSystem& jobs,
                                        const Luminumbra::IVec3& coords) {
    EnableSnapshotSession(reopened, root, jobs);
    ASSERT_TRUE(reopened.LoadWorld(session.GetMetadata().worldId)) << reopened.GetWorldOpenError();
    // The far chunk is disk-only here: reopened retention must use durable inventory.
    ASSERT_EQ(reopened.GetWorldSystem()->find_streamed_chunk(coords), nullptr);
    EXPECT_EQ(reopened.GetActiveRegionLedger().encode(), session.GetActiveRegionLedger().encode());
}

void AssertWaterRetentionRecords(GameSession& session,
                                 RegionKey water_region,
                                 const ActiveRegionRecord& frozen_record) {
    const auto& records = session.GetActiveRegionLedger().records();
    ASSERT_TRUE(records.contains(water_region));
    const auto& retained = records.at(water_region);
    EXPECT_EQ(retained.state, RegionState::Frozen);
    EXPECT_EQ(retained.frozen_digest, frozen_record.frozen_digest);
    EXPECT_EQ(retained.frozen_at, frozen_record.frozen_at);
    EXPECT_EQ(retained.last_proximity, frozen_record.last_proximity);
    EXPECT_FALSE(retained.pinned);
    EXPECT_FALSE(retained.edited);
    EXPECT_FALSE(retained.populated);
    EXPECT_EQ(retained.water_cursor, 0u);
    EXPECT_EQ(retained.pressure, 0u);
    // All visited neighbours lack authority and must have been evicted.
    EXPECT_EQ(records.size(), 2u);
}

void TickRegionalWaterRetention(GameSession& session,
                                GameSession& reopened,
                                RegionKey water_region,
                                RegionKey work_region,
                                const ActiveRegionRecord& frozen_record) {
    const auto dt = session.GetSimulationClock().fixed_dt();
    for (auto* active_session : {&session, &reopened})
        active_session->SetReplicatedSimulationAnchors({});
    for (int step = 0; step < 2; ++step) {
        for (auto* active_session : {&session, &reopened}) {
            active_session->RecordActiveRegionWork({work_region, 1, 0, 0, 0});
            ASSERT_EQ(active_session->TickSimulation(dt), 1u);
            ASSERT_FALSE(active_session->GetRegionSchedulerFault());
            ASSERT_NO_FATAL_FAILURE(
                AssertWaterRetentionRecords(*active_session, water_region, frozen_record));
        }
        EXPECT_EQ(reopened.GetRegionSchedule(), session.GetRegionSchedule());
        EXPECT_EQ(reopened.GetActiveRegionLedger().canonical_bytes(),
                  session.GetActiveRegionLedger().canonical_bytes());
    }
}

void AssertDurableWaterContinuation(GameSession& session,
                                    GameSession& reopened,
                                    const std::shared_ptr<Chunk>& chunk,
                                    const Luminumbra::IVec3& coords,
                                    RegionKey water_region,
                                    const ActiveRegionRecord& frozen_record,
                                    const FrozenWaterSnapshot& expected) {
    EXPECT_GT(session.GetWorldClock().tick(),
              frozen_record.last_proximity + ActiveRegionLedger::kIdleEvictionTicks);
    ASSERT_EQ(reopened.GetWorldSystem()->find_streamed_chunk(coords), nullptr);
    const auto durable_chunk = reopened.GetWorldSystem()->find_authoritative_chunk(coords);
    ASSERT_NE(durable_chunk, nullptr);
    EXPECT_EQ(durable_chunk->sdf_provenance(), ChunkSdfProvenance::LoadedOrEdited);
    EXPECT_EQ(durable_chunk->water_depth_mm, expected.depth);
    EXPECT_EQ(durable_chunk->water_bed_mm, expected.bed);
    EXPECT_EQ(durable_chunk->water_edge_flux, expected.flux);
    EXPECT_EQ(durable_chunk->is_water_sleeping.load(), chunk->is_water_sleeping.load());
    EXPECT_EQ(reopened.ComputeChunkStateHash(), session.ComputeChunkStateHash());
    EXPECT_EQ(
        P::WorldSaveService::region_simulation_digest(
            session.GetWorldSaveDir(),
            water_region,
            P::RegionDigestInputs{{}, {}, reopened.GetWorldSystem()->durable_chunk_inventory()}),
        frozen_record.frozen_digest);
}
} // namespace

TEST(GameSessionHeadlessWorldTest, DisplacementWaterPromotesBeforeFreezeAndSurvivesIdleReopen) {
    using Luminumbra::Chunk;
    using Luminumbra::ChunkState;
    using Luminumbra::IVec3;
    using Luminumbra::Vec3;
    using Luminumbra::world::RegionKey;
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    const RegionKey water_region{40, -40}, work_region{30, -40};
    const IVec3 coords(water_region.x * 32, 0, water_region.z * 32);
    const Vec3 visit_position(water_region.x * 512 + 256, 0, water_region.z * 512 + 256);
    const Vec3 displacement_position(coords.x * 16 + 8, 0, coords.z * 16 + 8);
    const auto empty_digest = P::WorldSaveService::region_simulation_digest({}, water_region, {});
    std::uint64_t generated_digest = 0;

    // Run the same seeded resident fixture with and without a production water writer.
    for (const bool displace : {false, true}) {
        SCOPED_TRACE(displace);
        RegionalWaterScenario scenario;
        auto& session = scenario.session;
        EnableSnapshotSession(session, root, jobs);
        session.SetRegionSchedulerConfig({0, 1, 1});
        ASSERT_TRUE(session.CreateWorld(
            displace ? "Displaced water" : "Generated water", "1337", "default"));
        auto chunk = std::make_shared<Chunk>(coords);
        chunk->set_state(ChunkState::Idle);
        chunk->sdf_data.assign(17 * 17 * 17, 1.0f);
        chunk->mark_sdf_generated_current_params();
        // Seed through the real initializer, keeping it asleep so this call only generates water.
        chunk->is_water_sleeping.store(true);
        ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
        std::vector<std::int32_t> generated_depth;
        ASSERT_NO_FATAL_FAILURE(SeedRegionalWaterFixture(scenario,
                                                         chunk,
                                                         water_region,
                                                         work_region,
                                                         visit_position,
                                                         empty_digest,
                                                         generated_depth));
        const auto initial_record = session.GetActiveRegionLedger().records().at(water_region);
        ASSERT_NO_FATAL_FAILURE(FreezeRegionalWaterFixture(scenario,
                                                           chunk,
                                                           displace,
                                                           water_region,
                                                           work_region,
                                                           displacement_position,
                                                           generated_depth,
                                                           initial_record));
        const auto frozen_record = session.GetActiveRegionLedger().records().at(water_region);
        if (!displace) {
            generated_digest = frozen_record.frozen_digest;
            EXPECT_EQ(generated_digest, empty_digest);
            EXPECT_EQ(chunk->water_depth_mm, generated_depth);
            EXPECT_FALSE(chunk->has_water_authority());
            EXPECT_FALSE(chunk->is_voxel_data_dirty());
            continue;
        }
        EXPECT_NE(frozen_record.frozen_digest, generated_digest);
        const FrozenWaterSnapshot evolved{
            chunk->water_depth_mm, chunk->water_bed_mm, chunk->water_edge_flux};
        ASSERT_NO_FATAL_FAILURE(PersistRegionalWaterBeforeRetention(
            session, chunk, work_region, coords, frozen_record));

        GameSession reopened;
        ASSERT_NO_FATAL_FAILURE(
            ReopenRegionalWaterBeforeRetention(session, reopened, root, jobs, coords));
        ASSERT_NO_FATAL_FAILURE(TickRegionalWaterRetention(
            session, reopened, water_region, work_region, frozen_record));
        ASSERT_NO_FATAL_FAILURE(AssertDurableWaterContinuation(
            session, reopened, chunk, coords, water_region, frozen_record, evolved));
        ASSERT_TRUE(session.SaveWorldState());
        GameSession after_retention;
        EnableSnapshotSession(after_retention, root, jobs);
        ASSERT_TRUE(after_retention.LoadWorld(session.GetMetadata().worldId))
            << after_retention.GetWorldOpenError();
        EXPECT_EQ(after_retention.GetWorldClock().tick(), session.GetWorldClock().tick());
        EXPECT_EQ(after_retention.GetActiveRegionLedger().encode(),
                  session.GetActiveRegionLedger().encode());
        EXPECT_EQ(after_retention.ComputeChunkStateHash(), session.ComputeChunkStateHash());
    }
}

TEST(GameSessionHeadlessWorldTest, FrozenRegionWaterChangeHashesCurrentAuthority) {
    using Luminumbra::Chunk;
    using Luminumbra::ChunkState;
    using Luminumbra::IVec3;
    using Luminumbra::Vec3;
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    const RegionKey water_region{40, -40}, work_region{30, -40};
    const IVec3 coords(water_region.x * 32, 0, water_region.z * 32);
    const Vec3 visit_position(water_region.x * 512 + 256, 0, water_region.z * 512 + 256);
    const Vec3 displacement_position(coords.x * 16 + 8, 0, coords.z * 16 + 8);
    const auto empty_digest = P::WorldSaveService::region_simulation_digest({}, water_region, {});
    RegionalWaterScenario scenario;
    auto& session = scenario.session;
    EnableSnapshotSession(session, root, jobs);
    session.SetRegionSchedulerConfig({0, 1, 1});
    ASSERT_TRUE(session.CreateWorld("Frozen water change", "1337", "default"));
    auto chunk = std::make_shared<Chunk>(coords);
    chunk->set_state(ChunkState::Idle);
    chunk->sdf_data.assign(std::size_t{17} * 17 * 17, 1.0f);
    chunk->mark_sdf_generated_current_params();
    chunk->is_water_sleeping.store(true);
    ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
    std::vector<std::int32_t> generated_depth;
    ASSERT_NO_FATAL_FAILURE(SeedRegionalWaterFixture(
        scenario, chunk, water_region, work_region, visit_position, empty_digest, generated_depth));
    const auto initial_record = session.GetActiveRegionLedger().records().at(water_region);
    ASSERT_NO_FATAL_FAILURE(FreezeRegionalWaterFixture(scenario,
                                                       chunk,
                                                       true,
                                                       water_region,
                                                       work_region,
                                                       displacement_position,
                                                       generated_depth,
                                                       initial_record));
    const auto frozen_hash = session.ComputeChunkStateHash();
    ASSERT_FALSE(session.GetActiveRegionLedger().records().at(water_region).wake_pending);

    // Resident water keeps evolving after the freeze; the frozen digest must not hide it.
    chunk->water_depth_mm.front() += 7;
    chunk->mark_water_state_dirty();
    EXPECT_EQ(session.ComputeChunkStateHash(), frozen_hash);
    EXPECT_TRUE(chunk->is_water_state_dirty());
    EXPECT_FALSE(session.GetActiveRegionLedger().records().at(water_region).wake_pending);
    // The water writer above ran outside a tick; explicitly complete its ownership boundary.
    session.GetWorldSystem()->sync_water_authority();
    const auto changed_hash = session.ComputeChunkStateHash();
    EXPECT_NE(changed_hash, frozen_hash);
    EXPECT_FALSE(chunk->is_water_state_dirty());
    const auto& woken = session.GetActiveRegionLedger().records().at(water_region);
    EXPECT_EQ(woken.state, RegionState::Frozen);
    EXPECT_TRUE(woken.wake_pending);
    EXPECT_EQ(session.ComputeChunkStateHash(), changed_hash);

    ASSERT_TRUE(session.SaveWorldState());
    EXPECT_EQ(session.ComputeChunkStateHash(), changed_hash);
    GameSession reopened;
    EnableSnapshotSession(reopened, root, jobs);
    ASSERT_TRUE(reopened.LoadWorld(session.GetMetadata().worldId)) << reopened.GetWorldOpenError();
    EXPECT_EQ(reopened.GetActiveRegionLedger().encode(), session.GetActiveRegionLedger().encode());
    EXPECT_EQ(reopened.ComputeChunkStateHash(), changed_hash);

    const auto dt = session.GetSimulationClock().fixed_dt();
    session.RecordActiveRegionWork({work_region, 1, 0, 0, 0});
    ASSERT_EQ(session.TickSimulation(dt), 1u);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    const auto& resumed = session.GetActiveRegionLedger().records().at(water_region);
    EXPECT_NE(resumed.state, RegionState::Frozen);
    EXPECT_FALSE(resumed.wake_pending);
    EXPECT_EQ(session.ComputeChunkStateHash(), changed_hash);
}

TEST(GameSessionHeadlessWorldTest, CanonicalSaveAfterAlternateLoadWritesCleanResidentAuthority) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    const Luminumbra::IVec3 coords(0, 0, 0);
    const auto alternate = root.path() / "alternate-authority";
    GameSession owner, adopter, reopened;
    for (auto* s : {&owner, &adopter, &reopened})
        EnableSnapshotSession(*s, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Alternate authority", "1337", "default"));
    auto chunk = std::make_shared<Luminumbra::Chunk>(coords);
    chunk->set_state(Luminumbra::ChunkState::Idle);
    chunk->heightmap_data = {8.5f};
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(chunk));
    ASSERT_TRUE(owner.SaveWorldStateTo(alternate));
    const auto save = owner.GetWorldSaveDir();
    // The export leaves the canonical save without this authority.
    ASSERT_NE(P::WorldSaveService::read_chunk(save, coords, true).status,
              P::WorldSaveService::ChunkReadStatus::Found);

    ASSERT_TRUE(adopter.LoadWorld(owner.GetMetadata().worldId)) << adopter.GetWorldOpenError();
    ASSERT_TRUE(adopter.LoadWorldStateFrom(alternate)) << adopter.GetWorldOpenError();
    auto* world = adopter.GetWorldSystem();
    ASSERT_TRUE(world->restore_authoritative_chunk(coords));
    const auto resident = world->find_authoritative_chunk(coords);
    ASSERT_NE(resident, nullptr);
    EXPECT_FALSE(resident->is_voxel_data_dirty());
    EXPECT_EQ(resident->heightmap_data, chunk->heightmap_data);

    // Retargeting the durable directory must first write clean resident authority there.
    Luminumbra::world::WorldStateSaveReport report;
    ASSERT_TRUE(adopter.SaveWorldState(&report));
    EXPECT_EQ(report.chunks_dirty, 0u);
    EXPECT_TRUE(report.saved);
    const auto written = P::WorldSaveService::read_chunk(save, coords, true);
    ASSERT_EQ(written.status, P::WorldSaveService::ChunkReadStatus::Found) << written.error;
    ASSERT_NE(written.chunk, nullptr);
    EXPECT_EQ(written.chunk->heightmap_data, chunk->heightmap_data);

    ASSERT_TRUE(reopened.LoadWorld(owner.GetMetadata().worldId)) << reopened.GetWorldOpenError();
    EXPECT_EQ(reopened.GetWorldSystem()->durable_chunk_inventory().size(), 1u);
    ASSERT_TRUE(reopened.GetWorldSystem()->restore_authoritative_chunk(coords));
    const auto restored = reopened.GetWorldSystem()->find_authoritative_chunk(coords);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->heightmap_data, chunk->heightmap_data);
    EXPECT_FALSE(reopened.GetRegionSchedulerFault());
}

TEST(GameSessionHeadlessWorldTest, LoadingAnotherSnapshotReplacesTheDurableInventory) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    const Luminumbra::IVec3 distant(1280, 0, -1280);
    const auto alternate = root.path() / "inventory-without-distant";
    GameSession owner, adopter, control;
    for (auto* s : {&owner, &adopter, &control})
        EnableSnapshotSession(*s, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Inventory replacement", "1337", "default"));
    ASSERT_TRUE(owner.SaveWorldStateTo(alternate));
    auto chunk = std::make_shared<Luminumbra::Chunk>(distant);
    chunk->sdf_data.assign(std::size_t{17} * 17 * 17, -4.0f);
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(chunk));
    ASSERT_TRUE(owner.SaveWorldState());
    ASSERT_EQ(P::WorldSaveService::read_chunk(owner.GetWorldSaveDir(), distant, true).status,
              P::WorldSaveService::ChunkReadStatus::Found);
    ASSERT_EQ(P::WorldSaveService::read_chunk(alternate, distant, true).status,
              P::WorldSaveService::ChunkReadStatus::Absent);

    ASSERT_TRUE(control.CreateTransientWorld("Control", "1337", "default"));
    ASSERT_TRUE(control.LoadWorldStateFrom(alternate)) << control.GetWorldOpenError();
    ASSERT_TRUE(adopter.LoadWorld(owner.GetMetadata().worldId)) << adopter.GetWorldOpenError();
    const auto canonical = adopter.GetWorldSystem()->durable_chunk_inventory();
    ASSERT_NE(std::find(canonical.begin(), canonical.end(), distant), canonical.end());
    ASSERT_FALSE(adopter.GetWorldSystem()->region_digest_inputs(40, -40).durable.empty());

    ASSERT_TRUE(adopter.LoadWorldStateFrom(alternate)) << adopter.GetWorldOpenError();
    auto* world = adopter.GetWorldSystem();
    const auto adopted = world->durable_chunk_inventory();
    EXPECT_EQ(adopted, control.GetWorldSystem()->durable_chunk_inventory());
    EXPECT_EQ(std::find(adopted.begin(), adopted.end(), distant), adopted.end());
    EXPECT_TRUE(world->region_digest_inputs(40, -40).durable.empty());
    // A member only the previous snapshot held is not expected authority of the adopted one.
    EXPECT_FALSE(world->restore_authoritative_chunk(distant));
    EXPECT_FALSE(adopter.GetRegionSchedulerFault().has_value());
}

TEST(GameSessionHeadlessWorldTest, WaterOnlyAuthorityHashDoesNotChangeWhenSaved) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner;
    EnableSnapshotSession(owner, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Water-only hash", "1337", "default"));
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    chunk->sdf_data.assign(17 * 17 * 17, 1.0f);
    chunk->mark_sdf_generated_current_params();
    chunk->water_depth_mm = {123};
    chunk->mark_water_state_dirty();
    ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(chunk));
    // Hash reads observe the last completed boundary; settle it as a finished tick would.
    owner.GetWorldSystem()->sync_water_authority();
    const auto before = owner.ComputeChunkStateHash();
    ASSERT_TRUE(owner.SaveWorldState());
    EXPECT_EQ(owner.ComputeChunkStateHash(), before);
    const auto restored = P::WorldSaveService::read_chunk(owner.GetWorldSaveDir(), {0, 0, 0}, true);
    ASSERT_EQ(restored.status, P::WorldSaveService::ChunkReadStatus::Found);
    ASSERT_NE(restored.chunk, nullptr);
    EXPECT_EQ(restored.chunk->water_depth_mm, chunk->water_depth_mm);
}

TEST(GameSessionHeadlessWorldTest, ExitSaveSkipsSimulatedButUnchangedWater) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    const Luminumbra::IVec3 coords(0, 0, 0);
    for (const bool changed : {false, true}) {
        SCOPED_TRACE(changed);
        RegionalWaterScenario scenario;
        auto& session = scenario.session;
        EnableSnapshotSession(session, root, jobs);
        ASSERT_TRUE(
            session.CreateWorld(changed ? "Changed water" : "Idle water", "1337", "default"));
        auto chunk = std::make_shared<Luminumbra::Chunk>(coords);
        chunk->set_state(Luminumbra::ChunkState::Idle);
        chunk->sdf_data.assign(std::size_t{17} * 17 * 17, 1.0f);
        chunk->mark_sdf_generated_current_params();
        chunk->is_water_sleeping.store(true);
        ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
        scenario.water_chunks.emplace(chunk->get_id(), chunk);
        auto* water = session.GetWaterSystem();
        water->update(session.GetRegistry(), scenario.water_chunks);
        ASSERT_EQ(chunk->water_depth_mm.size(), 64u);
        // Dry water at rest: every step keeps depth and flux and only advances the calm counter.
        chunk->water_bed_mm.assign(64, 4000);
        chunk->water_depth_mm.assign(64, 0);
        chunk->water_edge_flux.assign(128, 0);
        chunk->is_water_sleeping.store(false);
        const auto revision = chunk->water_revision();
        for (int tick = 0; tick < 130; ++tick)
            water->update(session.GetRegistry(), scenario.water_chunks);
        EXPECT_EQ(chunk->ticks_below_threshold, 120);
        EXPECT_TRUE(chunk->is_water_sleeping.load());
        EXPECT_EQ(chunk->water_revision(), revision);
        EXPECT_FALSE(chunk->needs_authority());
        if (changed)
            water->apply_displacement(Luminumbra::Vec3(8.0f, 0.0f, 8.0f), 4.0f);

        Luminumbra::world::WorldStateSaveReport report;
        ASSERT_TRUE(session.SaveWorldState(&report));
        EXPECT_EQ(report.chunks_dirty, changed ? 1u : 0u);
        EXPECT_EQ(chunk->has_water_authority(), changed);
        const auto durable =
            P::WorldSaveService::read_chunk(session.GetWorldSaveDir(), coords, true);
        if (!changed) {
            EXPECT_NE(durable.status, P::WorldSaveService::ChunkReadStatus::Found);
            continue;
        }
        ASSERT_EQ(durable.status, P::WorldSaveService::ChunkReadStatus::Found) << durable.error;
        ASSERT_NE(durable.chunk, nullptr);
        EXPECT_EQ(durable.chunk->water_depth_mm, chunk->water_depth_mm);
        EXPECT_EQ(durable.chunk->ticks_below_threshold, chunk->ticks_below_threshold);
        EXPECT_EQ(durable.chunk->is_water_sleeping.load(), chunk->is_water_sleeping.load());
    }
}

TEST(GameSessionHeadlessWorldTest, JoinedLedgerWriterDoesNotRetainCallerLocalReference) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner;
    EnableSnapshotSession(owner, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Ledger lifetime", "1337", "default"));
    const auto save = owner.GetWorldSaveDir();
    {
        P::SaveTransaction transaction(save, 0, {}, &P::WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(nullptr));
        {
            auto local = owner.GetActiveRegionLedger();
            local.mark_edited({10, 10}, owner.GetWorldClock());
            ASSERT_TRUE(
                P::WorldSaveService::save_active_regions(local, owner.GetWorldClock(), save));
        }
        ASSERT_TRUE(transaction.commit(nullptr));
    }
    Luminumbra::world::ActiveRegionLedger durable;
    ASSERT_TRUE(P::WorldSaveService::load_active_regions(durable, save));
    EXPECT_TRUE(durable.records().contains({10, 10}));
}

TEST(GameSessionHeadlessWorldTest, FarOverlayFilesDoNotChangeTheWorldHash) {
    using Luminumbra::World::FarBrickState;
    using Luminumbra::World::FarOverlayBrick;
    using Luminumbra::World::FarOverlayTile;
    using Luminumbra::World::FarTileKey;

    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original, loaded, reloaded;
    for (auto* session : {&original, &loaded, &reloaded}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
    }
    ASSERT_TRUE(original.CreateWorld("Far overlay", "1337", "default"));
    ASSERT_TRUE(original.SaveWorldState());
    const auto save = original.GetWorldSaveDir();
    const auto before = SessionWorldHash(original);

    FarOverlayTile near_tile;
    near_tile.key = FarTileKey{1, 0, 0};
    near_tile.generation = 1;
    near_tile.bricks.push_back(FarOverlayBrick{0, 0, 0, FarBrickState::HomogeneousAir});
    FarOverlayTile coarse_tile;
    coarse_tile.key = FarTileKey{3, -1, -1};
    coarse_tile.generation = 1;
    coarse_tile.bricks.push_back(FarOverlayBrick{-32, 1, -32, FarBrickState::HomogeneousSolid});
    ASSERT_TRUE(Luminumbra::World::WriteFarOverlay(save, near_tile));
    ASSERT_TRUE(Luminumbra::World::WriteFarOverlay(save, coarse_tile));
    ASSERT_TRUE(P::WorldSaveService::validate_save(save));
    ASSERT_TRUE(Luminumbra::World::ValidateFarOverlayDirectory(save));

    ASSERT_TRUE(loaded.LoadWorld(original.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_EQ(SessionWorldHash(loaded), before);

    fs::remove_all(save / "far");
    ASSERT_TRUE(reloaded.LoadWorld(original.GetMetadata().worldId)) << reloaded.GetWorldOpenError();
    EXPECT_EQ(SessionWorldHash(reloaded), before);
}

TEST(GameSessionHeadlessWorldTest, SharedSystemConfigPreservesExplicitOff) {
    GameSession session;
    const auto off = luminumbra::core::SystemConfig::FromJsonString(R"({"sim":{
        "active_regions":{"enabled":false}, "aether_state":{"enabled":false},
        "water_high_res":{"enabled":false}, "hydrology_weather":{"enabled":false},
        "weather_events":{"enabled":false}}})");
    session.ApplySystemConfig(off);
    EXPECT_FALSE(session.ActiveRegionsEnabled());
    EXPECT_FALSE(session.AetherStateEnabled());
    EXPECT_FALSE(session.WeatherRainEnabled());
    EXPECT_FALSE(session.WeatherEventsEnabled());
    // Finite hydrology is an independent shipping gate, not a tuning selector.
    EXPECT_TRUE(session.FiniteHydrologyEnabled());
    session.SetFiniteHydrologyEnabled(false);
    EXPECT_FALSE(session.FiniteHydrologyEnabled());
}

TEST(GameSessionHeadlessWorldTest, WaterRuleIdentitiesObserveIndependentSetters) {
    GameSession first, second;
    EXPECT_EQ(first.SystemConfigIdentity(), second.SystemConfigIdentity());
    first.SetFiniteHydrologyEnabled(false);
    EXPECT_NE(first.SystemConfigIdentity(), second.SystemConfigIdentity());
    second.SetFiniteHydrologyEnabled(false);
    EXPECT_EQ(first.SystemConfigIdentity(), second.SystemConfigIdentity());
    first.SetFiniteHydrologyEnabled(true);
    second.SetFiniteHydrologyEnabled(true);
    first.SetWeatherRainEnabled(true, 26);
    EXPECT_NE(first.SystemConfigIdentity(), second.SystemConfigIdentity());
    second.SetWeatherRainEnabled(true, 26);
    EXPECT_EQ(first.SystemConfigIdentity(), second.SystemConfigIdentity());
}

TEST(GameSessionHeadlessWorldTest, EffectiveIdentityUsesDefaultsAndCurrentSetters) {
    using luminumbra::core::SystemConfig;
    GameSession direct, applied;
    applied.ApplySystemConfig(SystemConfig::Defaults());
    const auto baseline = direct.SystemConfigIdentity();
    EXPECT_FALSE(baseline.empty());
    EXPECT_EQ(baseline, applied.SystemConfigIdentity());
    EXPECT_EQ(baseline, "35e233a89e8c87b1");
    EXPECT_EQ(direct.LegacyRevision3SystemConfigIdentity(), "60ab3c242d54ce89");
    for (const auto& [name, identity] : Luminumbra::TestSupport::EffectiveIdentityMismatches()) {
        SCOPED_TRACE(name);
        EXPECT_NE(identity, baseline);
    }
    // Turning off a tuning override with no nondefault tuning changes no effective rules.
    applied.ApplySystemConfig(SystemConfig::FromJsonString(
        R"({"sim":{"ecology":{"enabled":false,"params":{"herd_weight":0.9}}},
            "render":{"circadian":{"enabled":false,"params":{"amplitude":0.5}}}})"));
    EXPECT_EQ(applied.SystemConfigIdentity(), baseline);
    applied.ApplySystemConfig(SystemConfig::FromJsonString(
        R"({"sim":{"reproduction":{"params":{"maturity_ticks":90.75}}}})"));
    EXPECT_EQ(applied.SystemConfigIdentity(), baseline);
    const auto variants = Luminumbra::TestSupport::EffectiveIdentityMismatches();
    ASSERT_GE(variants.size(), 2u);
    EXPECT_NE(variants[variants.size() - 2].second, variants.back().second);
    applied.SetEcologyTuning(direct.GetEcologyTuning());
    direct.SetCircadianAmplitude(0.5f);
    applied.SetCircadianAmplitude(0.5f);
    EXPECT_EQ(direct.SystemConfigIdentity(), applied.SystemConfigIdentity());
}

TEST(GameSessionHeadlessWorldTest, CorruptLaterEditMemberLeavesEarlierAuthorityUnchanged) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, loaded;
    EnableSnapshotSession(owner, root, jobs);
    EnableSnapshotSession(loaded, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Edit admission", "1337", "default"));
    for (const int x : {31, 32}) {
        auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(x, 0, 0));
        chunk->sdf_data.assign(17 * 17 * 17, -4.0f);
        chunk->mark_voxel_data_dirty();
        ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(chunk));
    }
    ASSERT_TRUE(owner.SaveWorldState());
    ASSERT_TRUE(loaded.LoadWorld(owner.GetMetadata().worldId));
    auto* world = loaded.GetWorldSystem();
    ASSERT_TRUE(world->restore_authoritative_chunk({31, 0, 0}));
    const auto earlier = world->find_streamed_chunk({31, 0, 0});
    ASSERT_NE(earlier, nullptr);
    const auto before = earlier->sdf_data;
    const auto directory = root.path() / "worlds/saves" / owner.GetMetadata().worldId;
    ASSERT_TRUE(fs::remove(P::WorldSaveService::region_file_path(directory, 1, 0)));
    EXPECT_EQ(world->EditTerrainVoxel({512, 8, 8}, 2.0f, false, nullptr), 0);
    EXPECT_EQ(earlier->sdf_data, before);
    EXPECT_TRUE(loaded.GetRegionSchedulerFault().has_value());
    EXPECT_FALSE(world->PrepareTerrainVoxelEdit({512, 8, 8}, 2.0f));
    const auto resident_count = world->snapshot_streamed_chunks().size();
    world->regenerate_all_chunks(nullptr);
    EXPECT_EQ(world->snapshot_streamed_chunks().size(), resident_count);
    EXPECT_EQ(earlier->sdf_data, before);
    EXPECT_FALSE(world->find_streamed_chunk({32, 0, 0}));
}

TEST(GameSessionHeadlessWorldTest, FailedSaveAsRemovesOnlyItsNewPreset) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    EnableSnapshotSession(session, root, jobs);
    ASSERT_TRUE(session.CreateWorld("Preset rollback", "1337", "default"));
    const auto destination = root.path() / "preset-rollback";
    P::WorldSaveService::set_before_active_regions_replace_for_testing([] { return false; });
    const bool saved = session.SaveWorldStateTo(destination);
    P::WorldSaveService::set_before_active_regions_replace_for_testing(nullptr);
    EXPECT_FALSE(saved);
    EXPECT_FALSE(fs::exists(destination / "preset.json"));
    EXPECT_FALSE(fs::exists(destination / "world_info.json"));
    EXPECT_FALSE(fs::exists(P::snapshot_index_path(destination)));
    EXPECT_TRUE(fs::exists(session.GetWorldSaveDir() / "preset.json"));
    EXPECT_GE(session.GetPersistenceStatus().failures, 1u);
}

namespace {

constexpr std::uint32_t kGroundType = 7;
const Luminumbra::world::RegionKey kGroundRegion{0, -1};

using GroundEvents = std::vector<std::pair<std::string, std::string>>;

void CreateGroundWorld(GameSession& session,
                       const HeadlessRoot& root,
                       JobSystem& jobs,
                       const char* name) {
    EnableSnapshotSession(session, root, jobs);
    ASSERT_TRUE(session.CreateWorld(name, "1337", "default"));
}

std::shared_ptr<Luminumbra::Chunk> GroundSurfaceChunk(const Luminumbra::IVec3& coords,
                                                      float height) {
    auto chunk = std::make_shared<Luminumbra::Chunk>(coords);
    chunk->set_state(Luminumbra::ChunkState::Ready);
    chunk->current_lod.store(0);
    constexpr int nx = Luminumbra::CHUNK_SIZE_X + 1;
    constexpr int ny = Luminumbra::CHUNK_SIZE_Y + 1;
    constexpr int nz = Luminumbra::CHUNK_SIZE_Z + 1;
    chunk->sdf_data.resize(nx * ny * nz);
    for (int z = 0; z < nz; ++z) {
        for (int y = 0; y < ny; ++y) {
            for (int x = 0; x < nx; ++x) {
                chunk->sdf_data[static_cast<std::size_t>(x + nx * (y + ny * z))] =
                    static_cast<float>(coords.y * Luminumbra::CHUNK_SIZE_Y + y) - height;
            }
        }
    }
    chunk->mark_voxel_data_dirty();
    return chunk;
}

void RestoreUnsettledGroundObjects(GameSession& session) {
    Luminumbra::world::GroundObjectStore unsettled;
    for (const auto& [id, object] : session.GetGroundObjects().objects()) {
        auto waiting = object;
        waiting.flags = 0;
        waiting.y_mm += 10000;
        ASSERT_TRUE(unsettled.insert(waiting));
    }
    ASSERT_TRUE(session.SaveWorldState());
    ASSERT_TRUE(P::WorldSaveService::save_ground_objects(
        unsettled, session.GetWorldSaveDir(), nullptr, session.CaptureGroundNotifications()));
    ASSERT_TRUE(session.LoadWorldState()) << session.GetWorldOpenError();
}

// Spawns a ground object at (x_mm, 0, z_mm) with the fixed test type and payload.
std::uint64_t SpawnGround(GameSession& session,
                          std::int64_t x_mm,
                          std::int64_t z_mm,
                          std::uint64_t despawn_after = 0) {
    return session.SpawnGroundObject(kGroundType, x_mm, 0, z_mm, despawn_after, 0);
}

void TickOnce(GameSession& session) {
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
}

void CollectGroundEvents(GameSession& session, GroundEvents& seen) {
    session.GetSimulationEventBus().subscribe(
        [&seen](const auto& event) { seen.emplace_back(event.topic, event.payload); });
}

std::size_t CountTopic(const GroundEvents& seen, const std::string& topic) {
    return static_cast<std::size_t>(std::count_if(
        seen.begin(), seen.end(), [&topic](const auto& event) { return event.first == topic; }));
}

std::string FirstPayload(const GroundEvents& seen, const std::string& topic) {
    for (const auto& event : seen)
        if (event.first == topic)
            return event.second;
    return {};
}

std::size_t CountRegularFiles(const fs::path& dir) {
    std::size_t count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(dir))
        if (entry.is_regular_file())
            ++count;
    return count;
}

std::string MaskMetadataCreationTime(std::string bytes) {
    const auto creation_time = nlohmann::json::parse(bytes).at("creationTime").dump();
    const auto key = bytes.find("\"creationTime\"");
    const auto value = bytes.find_first_not_of(" \t\r\n", bytes.find(':', key) + 1);
    bytes.replace(value, creation_time.size(), "0");
    return bytes;
}

void ExpectSameSaveTree(const fs::path& actual, const fs::path& expected) {
    EXPECT_EQ(CountRegularFiles(actual), CountRegularFiles(expected));
    for (const auto& entry : fs::recursive_directory_iterator(actual)) {
        if (!entry.is_regular_file())
            continue;
        const auto relative = fs::relative(entry.path(), actual);
        auto actual_bytes = ReadClockTestFile(entry.path());
        auto expected_bytes = ReadClockTestFile(expected / relative);
        if (relative == "world_info.json") {
            actual_bytes = MaskMetadataCreationTime(std::move(actual_bytes));
            expected_bytes = MaskMetadataCreationTime(std::move(expected_bytes));
        }
        EXPECT_EQ(actual_bytes, expected_bytes) << relative.string();
    }
}

std::uint64_t GroundSpawnAdmissionLimit() {
    Luminumbra::world::GroundObjectStore store;
    std::size_t notification_bytes = 0;
    for (std::uint64_t id = 1; id <= Luminumbra::world::GroundObjectStore::kMaxObjects; ++id) {
        Luminumbra::world::GroundObject object;
        object.id = id;
        object.type = kGroundType;
        object.x_mm = 5200;
        object.z_mm = -1500;
        const luminumbra::simulation::SimulationEvent event{
            0, 0, id - 1, "ground.spawned", std::to_string(id) + ",7,5200,0,-1500"};
        notification_bytes += event.canonical_bytes().size() + 1;
        if (!store.insert(object, notification_bytes))
            return id - 1;
    }
    return store.size();
}

} // namespace

TEST(GameSessionHeadlessWorldTest, SaveComparisonMasksOnlyCreationTimeValue) {
    const std::string first = "{\n  \"creationTime\": 1793756527, \"seed\": 1793756527\n}";
    const std::string second = "{\n  \"creationTime\": 1793756528, \"seed\": 1793756527\n}";
    const std::string masked = "{\n  \"creationTime\": 0, \"seed\": 1793756527\n}";
    EXPECT_EQ(MaskMetadataCreationTime(first), masked);
    EXPECT_EQ(MaskMetadataCreationTime(second), masked);
    EXPECT_NE(MaskMetadataCreationTime("{\"creationTime\":1793756527,\"seed\":1793756527}"),
              masked);
    EXPECT_NE(
        MaskMetadataCreationTime("{\n  \"creationTime\": 1793756527, \"seed\": 1793756528\n}"),
        masked);
}

TEST(GameSessionHeadlessWorldTest, FlagOffSpawnIsInertAndLeavesSaveHashAndRejectionsUnchanged) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, expected;
    for (auto* s : {&session, &expected}) {
        s->SetRootPath(root.root_string());
        s->SetJobSystem(&jobs);
        s->SetActiveRegionsEnabled(false);
        ASSERT_TRUE(s->CreateWorld("Ground off", "1337", "default"));
        ASSERT_NO_FATAL_FAILURE(AdoptRegionGoldenChunk(*s, 0));
    }
    EXPECT_EQ(SpawnGround(session, 5200, -1500), 0u);
    EXPECT_EQ(SpawnGround(session, 5300, -1500), 0u);
    EXPECT_TRUE(session.GetGroundObjects().empty());
    for (int i = 0; i < 7; ++i) {
        ASSERT_NO_FATAL_FAILURE(TickOnce(session));
        ASSERT_NO_FATAL_FAILURE(TickOnce(expected));
    }
    const auto save = root.path() / "ground-off-save";
    const auto twin = root.path() / "ground-off-twin";
    ASSERT_TRUE(session.SaveWorldStateTo(save));
    ASSERT_TRUE(expected.SaveWorldStateTo(twin));
    ExpectSameSaveTree(save, twin);
    ExpectSameWorldHash(session, expected);
    const auto hashes = SessionWorldSubHashes(session);
    const auto expected_hashes = SessionWorldSubHashes(expected);
    EXPECT_EQ(hashes.at("ecology"), expected_hashes.at("ecology"));
    EXPECT_EQ(session.GetRegionInputRejections().ground_spawns, 0u);
    EXPECT_EQ(session.GetRegionInputRejections().edits, expected.GetRegionInputRejections().edits);
}

TEST(GameSessionHeadlessWorldTest, FlagOnSpawnReturnsSequentialIdsAndPublishesGroundSpawned) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    CreateGroundWorld(session, root, jobs, "Ground spawn");
    GroundEvents seen;
    CollectGroundEvents(session, seen);
    EXPECT_EQ(SpawnGround(session, 5200, -1500), 1u);
    EXPECT_EQ(SpawnGround(session, 5300, -1500), 2u);
    EXPECT_EQ(SpawnGround(session, 5400, -1500), 3u);
    // Events published between ticks are delivered by the next drain.
    EXPECT_TRUE(seen.empty());
    TickOnce(session);
    EXPECT_EQ(CountTopic(seen, "ground.spawned"), 3u);
    EXPECT_EQ(FirstPayload(seen, "ground.spawned"), "1,7,5200,0,-1500");
    EXPECT_EQ(session.GetGroundObjects().size(), 3u);
}

TEST(GameSessionHeadlessWorldTest, RefusedGroundSpawnsCountRejectionsAndConsumeNoId) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    CreateGroundWorld(session, root, jobs, "Ground refused");
    EXPECT_EQ(SpawnGround(session, Luminumbra::world::GroundObjectStore::kMaxCoordinateMm, -1500),
              0u);
    EXPECT_EQ(session.SpawnGroundObject(0, 5200, 0, -1500, 0, 0), 0u);
    EXPECT_EQ(session.GetRegionInputRejections().ground_spawns, 2u);
    EXPECT_TRUE(session.GetGroundObjects().empty());
    EXPECT_EQ(SpawnGround(session, 5200, -1500), 1u);
}

TEST(GameSessionHeadlessWorldTest, GroundSpawnRegionAdmissionRetainsSaveableLedger) {
    using Luminumbra::world::ActiveRegionLedger;
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, loaded;
    CreateGroundWorld(owner, root, jobs, "Ground region admission");
    ActiveRegionLedger ledger;
    const auto limit = ActiveRegionLedger::kMaxRegions - 1;
    for (std::uint32_t index = 0; index + 1 < limit; ++index) {
        ledger.mark_edited(
            {static_cast<std::int32_t>(index % 1024u), static_cast<std::int32_t>(index / 1024u)},
            owner.GetWorldClock());
    }
    ASSERT_TRUE(P::WorldSaveService::save_active_regions(
        ledger, owner.GetWorldClock(), owner.GetWorldSaveDir()));
    EnableSnapshotSession(loaded, root, jobs);
    ASSERT_TRUE(loaded.LoadWorld(owner.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    ASSERT_EQ(loaded.GetActiveRegionLedger().records().size(), limit - 1);
    ASSERT_EQ(SpawnGround(loaded, 5200, -1500), 1u);
    ASSERT_EQ(loaded.GetActiveRegionLedger().records().size(), limit);
    const auto notifications = loaded.CaptureGroundNotifications().canonical_bytes();
    EXPECT_EQ(SpawnGround(loaded, 600000, -1500), 0u);
    EXPECT_EQ(loaded.GetRegionInputRejections().ground_spawns, 1u);
    EXPECT_EQ(loaded.GetActiveRegionLedger().records().size(), limit);
    EXPECT_EQ(loaded.GetGroundObjects().size(), 1u);
    EXPECT_EQ(loaded.CaptureGroundNotifications().canonical_bytes(), notifications);
    EXPECT_FALSE(loaded.GetRegionSchedulerFault().has_value());
    EXPECT_EQ(SpawnGround(loaded, 5300, -1500), 3u);
    // Saving a million records is the expensive step, so the admitted, refused and
    // follow-up states are all verified by this one save.
    ASSERT_TRUE(loaded.SaveWorld());
}

TEST(GameSessionHeadlessWorldTest, LoadedGroundNotificationCursorNeverWrapsOnSpawn) {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    for (const auto cursor : {maximum, maximum - 1}) {
        SCOPED_TRACE(cursor);
        const HeadlessRoot root;
        JobSystem jobs;
        jobs.startup(1);
        GameSession owner, loaded, reloaded;
        CreateGroundWorld(owner, root, jobs, "Ground sequence admission");
        owner.GetSimulationEventBus().restore_pending({}, cursor);
        ASSERT_TRUE(owner.SaveWorld());
        EnableSnapshotSession(loaded, root, jobs);
        ASSERT_TRUE(loaded.LoadWorld(owner.GetMetadata().worldId)) << loaded.GetWorldOpenError();
        ASSERT_EQ(loaded.GetSimulationEventBus().next_sequence(), cursor);
        if (cursor < maximum) {
            ASSERT_EQ(SpawnGround(loaded, 5200, -1500), 1u);
            const auto pending = loaded.CaptureGroundNotifications();
            ASSERT_EQ(pending.events.size(), 1u);
            EXPECT_EQ(pending.events.front().sequence, maximum - 1);
            EXPECT_EQ(pending.next_sequence, maximum);
        }
        ASSERT_TRUE(loaded.SaveWorld());
        EnableSnapshotSession(reloaded, root, jobs);
        ASSERT_TRUE(reloaded.LoadWorld(owner.GetMetadata().worldId))
            << reloaded.GetWorldOpenError();
        EXPECT_EQ(reloaded.CaptureGroundNotifications().canonical_bytes(),
                  loaded.CaptureGroundNotifications().canonical_bytes());
        for (auto* session : {&loaded, &reloaded}) {
            const auto next_id = session->GetActiveRegionLedger().next_persistent_id();
            const auto notifications = session->CaptureGroundNotifications().canonical_bytes();
            EXPECT_EQ(SpawnGround(*session, 5300, -1500), 0u);
            EXPECT_EQ(session->GetRegionInputRejections().ground_spawns, 1u);
            EXPECT_EQ(session->GetActiveRegionLedger().next_persistent_id(), next_id);
            EXPECT_EQ(session->GetSimulationEventBus().next_sequence(), maximum);
            EXPECT_EQ(session->GetGroundObjects().size(), cursor < maximum ? 1u : 0u);
            EXPECT_EQ(session->CaptureGroundNotifications().canonical_bytes(), notifications);
            EXPECT_TRUE(session->SaveWorld());
        }
    }
}

TEST(GameSessionHeadlessWorldTest,
     ExhaustedGroundNotificationCursorLoadsButInvalidCursorRefusesLoad) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, loaded;
    CreateGroundWorld(owner, root, jobs, "Ground exhausted cursor");
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    owner.GetSimulationEventBus().restore_pending({}, maximum);
    ASSERT_TRUE(owner.SaveWorld());
    EnableSnapshotSession(loaded, root, jobs);
    ASSERT_TRUE(loaded.LoadWorld(owner.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_EQ(loaded.GetSimulationEventBus().next_sequence(), maximum);
    const auto save = owner.GetWorldSaveDir();
    const auto valid_bytes = owner.GetGroundObjects().serialize({maximum, {}});
    const auto cursor_text = std::to_string(maximum);
    const auto cursor_offset = valid_bytes.find(cursor_text);
    ASSERT_NE(cursor_offset, std::string::npos);
    P::SnapshotIndex index;
    ASSERT_TRUE(P::read_snapshot_index(save, index, nullptr));
    const auto member =
        std::find_if(index.members.begin(), index.members.end(), [](const auto& value) {
            return value.path == Luminumbra::world::GroundObjectStore::kRelativePath;
        });
    ASSERT_NE(member, index.members.end());
    for (const auto* invalid_cursor : {"18446744073709551616", "-1", "1.5"}) {
        SCOPED_TRACE(invalid_cursor);
        auto bytes = valid_bytes;
        bytes.replace(cursor_offset, cursor_text.size(), invalid_cursor);
        member->bytes = bytes.size();
        member->fnv64 = P::fnv1a64_bytes(bytes.data(), bytes.size());
        // Keep the fingerprint consistent so admission reaches the notification parser.
        std::ofstream(P::WorldSaveService::ground_objects_path(save), std::ios::binary) << bytes;
        std::ofstream(P::snapshot_index_path(save), std::ios::binary)
            << P::encode_snapshot_index(index);
        Luminumbra::world::GroundObjectStore restored;
        Luminumbra::world::GroundNotifications notifications{maximum, {}};
        std::vector<std::string> errors;
        EXPECT_FALSE(P::WorldSaveService::load_ground_objects(
            restored, save, 1, &errors, {}, &notifications));
        ASSERT_FALSE(errors.empty());
        EXPECT_NE(errors.front().find("malformed notifications"), std::string::npos);
        EXPECT_EQ(restored.serialize(notifications), valid_bytes);
        EXPECT_FALSE(loaded.LoadWorld(owner.GetMetadata().worldId));
        EXPECT_FALSE(loaded.GetWorldOpenError().empty());
        EXPECT_TRUE(loaded.GetGroundObjects().empty());
        EXPECT_EQ(loaded.GetSimulationEventBus().next_sequence(), maximum);
    }
}

TEST(GameSessionHeadlessWorldTest, ExhaustedGroundDespawnSequenceLatchesFaultWithoutWrapping) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, loaded;
    CreateGroundWorld(owner, root, jobs, "Ground despawn exhaustion");
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    owner.GetSimulationEventBus().restore_pending({}, maximum - 1);
    ASSERT_EQ(SpawnGround(owner, 5200, -1500, 1), 1u);
    ASSERT_TRUE(owner.SaveWorld());
    EnableSnapshotSession(loaded, root, jobs);
    ASSERT_TRUE(loaded.LoadWorld(owner.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_EQ(loaded.GetSimulationEventBus().next_sequence(), maximum);
    GroundEvents seen;
    CollectGroundEvents(loaded, seen);
    const auto objects = loaded.GetGroundObjects().serialize();
    ASSERT_NO_FATAL_FAILURE(TickOnce(loaded));
    EXPECT_TRUE(loaded.GetRegionSchedulerFault().has_value());
    EXPECT_EQ(loaded.GetSimulationEventBus().next_sequence(), maximum);
    EXPECT_EQ(loaded.GetGroundObjects().serialize(), objects);
    // The spawn used the last sequence; the despawn is refused instead of wrapping the cursor.
    EXPECT_EQ(CountTopic(seen, "ground.despawned"), 0u);
    // A latched fault retires pending notifications, so nothing is captured or persisted.
    const auto pending = loaded.CaptureGroundNotifications();
    EXPECT_EQ(pending.next_sequence, 0u);
    EXPECT_TRUE(pending.events.empty());
    ASSERT_TRUE(loaded.SaveWorld());
}

TEST(GameSessionHeadlessWorldTest, InsufficientGroundDespawnCapacityPreservesAllDueRegions) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    CreateGroundWorld(session, root, jobs, "Ground despawn capacity");
    ASSERT_EQ(SpawnGround(session, 5200, -1500, 1), 1u);
    ASSERT_EQ(SpawnGround(session, 600000, -1500, 1), 2u);
    session.GetSimulationEventBus().restore_pending({},
                                                    std::numeric_limits<std::uint64_t>::max() - 1);
    const auto objects = session.GetGroundObjects().serialize();
    GroundEvents seen;
    CollectGroundEvents(session, seen);
    ASSERT_NO_FATAL_FAILURE(TickOnce(session));
    EXPECT_TRUE(session.GetRegionSchedulerFault().has_value());
    EXPECT_EQ(session.GetGroundObjects().serialize(), objects);
    EXPECT_EQ(session.GetSimulationEventBus().next_sequence(),
              std::numeric_limits<std::uint64_t>::max() - 1);
    EXPECT_EQ(CountTopic(seen, "ground.despawned"), 0u);
}

TEST(GameSessionHeadlessWorldTest, GroundSpawnBurstRemainsSaveableAtAdmissionLimit) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, loaded;
    CreateGroundWorld(session, root, jobs, "Ground admission");
    const auto limit = GroundSpawnAdmissionLimit();
    ASSERT_GT(limit, 1u);
    ASSERT_LT(limit, Luminumbra::world::GroundObjectStore::kMaxObjects);
    for (std::uint64_t id = 1; id < limit; ++id) {
        ASSERT_EQ(SpawnGround(session, 5200, -1500), id);
    }
    ASSERT_TRUE(session.SaveWorldState());
    EXPECT_EQ(session.GetGroundObjects().size(), limit - 1);
    ASSERT_EQ(SpawnGround(session, 5200, -1500), limit);
    ASSERT_TRUE(session.SaveWorldState());
    const auto hash = SessionWorldHash(session);
    const auto notifications = session.CaptureGroundNotifications().canonical_bytes();
    EXPECT_EQ(SpawnGround(session, 5200, -1500), 0u);
    EXPECT_EQ(session.GetRegionInputRejections().ground_spawns, 1u);
    EXPECT_EQ(session.GetGroundObjects().size(), limit);
    EXPECT_EQ(session.CaptureGroundNotifications().canonical_bytes(), notifications);
    ASSERT_TRUE(session.SaveWorldState());
    EnableSnapshotSession(loaded, root, jobs);
    ASSERT_TRUE(loaded.LoadWorld(session.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(session));
    EXPECT_EQ(loaded.CaptureGroundNotifications().canonical_bytes(), notifications);
    // A refused spawn consumes an id, but does not enqueue an event or invalidate the save.
    EXPECT_NE(SessionWorldHash(session), hash);
    TickOnce(session);
    EXPECT_FALSE(session.GetRegionSchedulerFault().has_value());
    EXPECT_NE(SpawnGround(session, 5200, -1500), 0u);
    EXPECT_TRUE(session.SaveWorldState());
}

TEST(GameSessionHeadlessWorldTest, UnknownRegionWorkFromEventCallbackIsRejectedAndKeepsRunning) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    CreateGroundWorld(session, root, jobs, "Unknown callback work");
    const Luminumbra::world::RegionKey unknown{300, -300};
    ASSERT_FALSE(session.GetActiveRegionLedger().records().contains(unknown));
    ASSERT_EQ(SpawnGround(session, 5200, -1500), 1u);
    session.GetSimulationEventBus().subscribe([&](const auto&) {
        session.RecordActiveRegionWork({unknown, 1, 0, 0, 0});
        session.RecordActiveRegionWork({kGroundRegion, 0, 2, 0, 0});
    });
    TickOnce(session);
    ASSERT_FALSE(session.GetRegionSchedulerFault().has_value());
    EXPECT_EQ(session.GetActiveRegionLedger().completed_work().at(kGroundRegion), 3u);
    TickOnce(session);
    EXPECT_EQ(session.GetRegionSchedule().rejected.work, 1u);
    EXPECT_EQ(session.GetRegionSchedule().work, 3u);
    EXPECT_FALSE(session.GetActiveRegionLedger().records().contains(unknown));
    TickOnce(session);
    EXPECT_EQ(session.GetRegionSchedule().rejected.work, 0u);
    EXPECT_EQ(session.GetSimulationTickCount(), 3u);
    EXPECT_FALSE(session.GetRegionSchedulerFault().has_value());
    EXPECT_TRUE(session.SaveWorldState());
}

TEST(GameSessionHeadlessWorldTest, SpawnActivatesItsRegionWithoutTerrainEdit) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    CreateGroundWorld(session, root, jobs, "Ground activation");
    ASSERT_EQ(SpawnGround(session, 5200, -1500), 1u);
    const auto& records = session.GetActiveRegionLedger().records();
    ASSERT_EQ(records.count(kGroundRegion), 1u);
    EXPECT_FALSE(records.at(kGroundRegion).edited);
    EXPECT_TRUE(records.at(kGroundRegion).wake_pending);
    EXPECT_EQ(records.size(), 1u);
    auto ledger = session.GetActiveRegionLedger();
    const auto owned = [&](Luminumbra::world::RegionKey key) {
        return session.GetGroundObjects().owns_region(key);
    };
    ledger.schedule(Luminumbra::world::WorldClock(ledger.kIdleEvictionTicks), {}, {}, {}, owned);
    EXPECT_TRUE(ledger.records().contains(kGroundRegion));
}

TEST(GameSessionHeadlessWorldTest, DespawnAfterRemovesObjectAndPublishesGroundDespawned) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    CreateGroundWorld(session, root, jobs, "Ground despawn");
    GroundEvents seen;
    CollectGroundEvents(session, seen);
    const auto id = SpawnGround(session, 5200, -1500, 3);
    ASSERT_EQ(id, 1u);
    TickOnce(session);
    TickOnce(session);
    ASSERT_EQ(session.GetGroundObjects().objects().count(id), 1u);
    EXPECT_EQ(session.GetGroundObjects().objects().at(id).active_age, 2u);
    TickOnce(session);
    EXPECT_TRUE(session.GetGroundObjects().objects().empty());
    // The due update publishes the despawn before this tick's ordered drain.
    EXPECT_EQ(CountTopic(seen, "ground.despawned"), 1u);
    EXPECT_EQ(FirstPayload(seen, "ground.despawned"), "1,7,5200,0,-1500");
}

TEST(GameSessionHeadlessWorldTest, FrozenRegionDoesNotAgeItsGroundObjects) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    EnableSnapshotSession(session, root, jobs);
    session.SetRegionSchedulerConfig({0, 2, 1});
    ASSERT_TRUE(session.CreateWorld("Ground frozen", "1337", "default"));
    const auto id = SpawnGround(session, 5200, -1500);
    ASSERT_EQ(id, 1u);
    session.SetReplicatedSimulationAnchors({}); // No proximity protection for this pressure case.
    std::size_t frozen_ticks = 0;
    for (int i = 0; i < 10; ++i) {
        const auto before = session.GetGroundObjects().objects().at(id).active_age;
        session.RecordActiveRegionWork({kGroundRegion, 100, 0, 0, 0});
        TickOnce(session);
        // Frozen after a tick means it was not due in that tick, so its objects did not age.
        if (session.GetActiveRegionLedger().records().at(kGroundRegion).state ==
            Luminumbra::world::RegionState::Frozen) {
            ++frozen_ticks;
            EXPECT_EQ(session.GetGroundObjects().objects().at(id).active_age, before)
                << "tick " << i;
        }
    }
    EXPECT_GT(frozen_ticks, 0u);
}

TEST(GameSessionHeadlessWorldTest, MetadataOnlySaveAfterSpawnWritesObjectFileAndLedger) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    CreateGroundWorld(session, root, jobs, "Ground metadata");
    const auto save = session.GetWorldSaveDir();
    P::SnapshotIndex initial;
    ASSERT_TRUE(P::read_snapshot_index(save, initial, nullptr));
    const auto absent =
        std::find_if(initial.members.begin(), initial.members.end(), [](const auto& member) {
            return member.path == Luminumbra::world::GroundObjectStore::kRelativePath;
        });
    // A never-written member may be omitted; a deleted member is explicitly absent.
    if (absent != initial.members.end()) {
        EXPECT_FALSE(absent->present);
    }
    EXPECT_FALSE(fs::exists(P::WorldSaveService::ground_objects_path(save)));
    Luminumbra::world::GroundObjectStore restored;
    Luminumbra::world::GroundNotifications notifications;
    ASSERT_TRUE(P::WorldSaveService::load_ground_objects(
        restored,
        save,
        session.GetActiveRegionLedger().next_persistent_id(),
        nullptr,
        {},
        &notifications));
    EXPECT_TRUE(restored.empty());
    EXPECT_TRUE(notifications.events.empty());
    EXPECT_EQ(notifications.next_sequence, 0u);
    ASSERT_EQ(SpawnGround(session, 5200, -1500), 1u);
    ASSERT_EQ(SpawnGround(session, 5300, -1500), 2u);
    ASSERT_TRUE(session.SaveWorld());
    P::SnapshotIndex committed;
    ASSERT_TRUE(P::read_snapshot_index(save, committed, nullptr));
    const auto present =
        std::find_if(committed.members.begin(), committed.members.end(), [](const auto& member) {
            return member.path == Luminumbra::world::GroundObjectStore::kRelativePath;
        });
    ASSERT_NE(present, committed.members.end());
    EXPECT_TRUE(present->present);
    ASSERT_TRUE(P::WorldSaveService::validate_save(save));
    std::string error;
    ASSERT_TRUE(Luminumbra::world::GroundObjectStore::parse(
        ReadClockTestFile(P::WorldSaveService::ground_objects_path(save)),
        session.GetActiveRegionLedger().next_persistent_id(),
        restored,
        error,
        &notifications))
        << error;
    EXPECT_EQ(restored.objects(), session.GetGroundObjects().objects());
    EXPECT_EQ(notifications.events.size(), 2u);
    EXPECT_TRUE(fs::exists(P::WorldSaveService::active_regions_path(save)));
}

TEST(GameSessionHeadlessWorldTest, InvalidGroundNotificationsRefuseSaveWithoutChangingDisk) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    CreateGroundWorld(session, root, jobs, "Ground notification admission");
    ASSERT_EQ(SpawnGround(session, 5200, -1500), 1u);
    ASSERT_TRUE(session.SaveWorld());
    const auto save = session.GetWorldSaveDir();
    const auto ground = ReadClockTestFile(P::WorldSaveService::ground_objects_path(save));
    const auto index = ReadClockTestFile(P::snapshot_index_path(save));
    const std::vector<Luminumbra::world::GroundNotifications> invalid{
        {1, {{0, 0, 1, "ground.spawned", "equal cursor"}}},
        {1, {{0, 0, 2, "ground.spawned", "above cursor"}}},
        {1, {{0, 0, 0, "script.trigger", "unsupported topic"}}},
        {1, {{0, 0, 0, "ground.spawned", "first"}, {0, 0, 0, "ground.despawned", "duplicate"}}},
        {2, {{0, 0, 1, "ground.spawned", "first"}, {0, 0, 0, "ground.despawned", "unordered"}}},
        {std::numeric_limits<std::uint64_t>::max(),
         {{0, 0, std::numeric_limits<std::uint64_t>::max(), "ground.spawned", "equal cursor"}}},
        {1,
         {{0,
           0,
           0,
           "ground.spawned",
           std::string(Luminumbra::world::GroundObjectStore::kMaxFileBytes, 'x')}}}};
    for (const bool joined : {false, true}) {
        SCOPED_TRACE(joined);
        std::unique_ptr<P::SaveTransaction> transaction;
        if (joined) {
            transaction = std::make_unique<P::SaveTransaction>(
                save, 0, std::vector<std::array<float, 3>>{}, &P::WorldSaveService::member_digest);
            ASSERT_TRUE(transaction->begin(nullptr));
        }
        for (std::size_t i = 0; i < invalid.size(); ++i) {
            SCOPED_TRACE(i);
            std::vector<std::string> errors;
            EXPECT_FALSE(P::WorldSaveService::save_ground_objects(
                session.GetGroundObjects(), save, &errors, invalid[i]));
            EXPECT_FALSE(errors.empty());
            EXPECT_EQ(ReadClockTestFile(P::WorldSaveService::ground_objects_path(save)), ground);
            EXPECT_EQ(ReadClockTestFile(P::snapshot_index_path(save)), index);
        }
    }
}

namespace Luminumbra::world {

class GameSessionGroundMetadataTest : public testing::Test {
protected:
    static bool ReconcileMetadata(GameSession& session, const fs::path& save_dir) {
        return session.ReconcileWorldStateMetadataTo(save_dir);
    }
};

TEST_F(GameSessionGroundMetadataTest, GroundMetadataSaveCommitsOneGeneration) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    ASSERT_NO_FATAL_FAILURE(CreateGroundWorld(session, root, jobs, "Ground metadata transaction"));
    const auto save = session.GetWorldSaveDir();
    ASSERT_EQ(SpawnGround(session, 5200, -1500), 1u);
    ASSERT_EQ(P::SaveTransaction::active_for(save), nullptr);
    ASSERT_TRUE(ReconcileMetadata(session, save));
    ASSERT_TRUE(P::WorldSaveService::validate_save(save));
    const auto old_objects = session.GetGroundObjects().objects();
    const auto old_notifications = session.CaptureGroundNotifications().canonical_bytes();
    const std::array<const char*, 3> members{
        GroundObjectStore::kRelativePath, "world_info.json", "chunks/region/active-regions.arl"};
    std::map<std::string, std::string> previous;
    for (const auto* member : members)
        previous.emplace(member, ReadClockTestFile(save / member));
    const auto old_index = ReadClockTestFile(P::snapshot_index_path(save));
    P::SnapshotIndex initial;
    ASSERT_TRUE(P::read_snapshot_index(save, initial, nullptr));

    ASSERT_NO_FATAL_FAILURE(TickOnce(session));
    ASSERT_EQ(SpawnGround(session, 5300, -1500), 2u);
    ASSERT_EQ(P::SaveTransaction::active_for(save), nullptr);
    static bool ledger_replacement_attempted;
    ledger_replacement_attempted = false;
    P::WorldSaveService::set_before_active_regions_replace_for_testing([] {
        ledger_replacement_attempted = true;
        return false;
    });
    const bool saved = ReconcileMetadata(session, save);
    P::WorldSaveService::set_before_active_regions_replace_for_testing(nullptr);
    EXPECT_FALSE(saved);
    EXPECT_TRUE(ledger_replacement_attempted);
    ASSERT_TRUE(P::WorldSaveService::validate_save(save));
    for (const auto& [member, bytes] : previous) {
        EXPECT_EQ(ReadClockTestFile(save / member), bytes) << member;
    }
    EXPECT_EQ(ReadClockTestFile(P::snapshot_index_path(save)), old_index);
    EXPECT_FALSE(P::journal_pending(save));
    ActiveRegionLedger ledger;
    ASSERT_TRUE(P::WorldSaveService::load_active_regions(ledger, save));
    EXPECT_EQ(ledger.next_persistent_id(), 2u);
    GroundObjectStore objects;
    GroundNotifications notifications;
    ASSERT_TRUE(P::WorldSaveService::load_ground_objects(
        objects, save, ledger.next_persistent_id(), nullptr, {}, &notifications));
    EXPECT_EQ(objects.objects(), old_objects);
    EXPECT_EQ(notifications.canonical_bytes(), old_notifications);

    ASSERT_TRUE(ReconcileMetadata(session, save));
    ASSERT_TRUE(P::WorldSaveService::validate_save(save));
    P::SnapshotIndex committed;
    ASSERT_TRUE(P::read_snapshot_index(save, committed, nullptr));
    EXPECT_EQ(committed.generation, initial.generation + 1);
    for (const auto* member : members) {
        const auto found =
            std::find_if(committed.members.begin(),
                         committed.members.end(),
                         [member](const auto& entry) { return entry.path == member; });
        ASSERT_NE(found, committed.members.end()) << member;
        EXPECT_TRUE(found->present) << member;
        EXPECT_EQ(found->generation, committed.generation) << member;
        EXPECT_NE(ReadClockTestFile(save / member), previous.at(member)) << member;
    }
    ASSERT_TRUE(P::WorldSaveService::load_active_regions(ledger, save));
    EXPECT_EQ(ledger.next_persistent_id(), 3u);
    EXPECT_EQ(ledger.header_tick(), session.GetSimulationTickCount());
    WorldClock clock;
    bool requires_active_regions = false;
    ASSERT_TRUE(P::WorldSaveService::read_clock_metadata(save, clock, requires_active_regions));
    EXPECT_TRUE(requires_active_regions);
    EXPECT_EQ(clock.tick(), session.GetSimulationTickCount());
    ASSERT_TRUE(P::WorldSaveService::load_ground_objects(
        objects, save, ledger.next_persistent_id(), nullptr, {}, &notifications));
    EXPECT_EQ(objects.objects(), session.GetGroundObjects().objects());
    EXPECT_EQ(notifications.canonical_bytes(),
              session.CaptureGroundNotifications().canonical_bytes());
    EXPECT_FALSE(P::journal_pending(save));
}

TEST_F(GameSessionGroundMetadataTest, EmptyGroundMetadataSavePreservesNotifications) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, loaded;
    ASSERT_NO_FATAL_FAILURE(CreateGroundWorld(session, root, jobs, "Empty ground metadata"));
    const auto save = session.GetWorldSaveDir();
    const auto ground_path = P::WorldSaveService::ground_objects_path(save);
    ASSERT_TRUE(session.GetGroundObjects().empty());
    ASSERT_TRUE(session.CaptureGroundNotifications().canonical_bytes().empty());
    ASSERT_EQ(P::SaveTransaction::active_for(save), nullptr);
    ASSERT_TRUE(ReconcileMetadata(session, save));
    EXPECT_FALSE(fs::exists(ground_path));

    GroundEvents saved_seen;
    CollectGroundEvents(session, saved_seen);
    session.GetSimulationEventBus().publish(0, "ground.spawned", "first", 1);
    session.GetSimulationEventBus().publish(0, "ground.despawned", "second", -1);
    session.GetSimulationEventBus().publish(0, "ground.spawned", "third", 1);
    const auto notifications = session.CaptureGroundNotifications().canonical_bytes();
    ASSERT_FALSE(notifications.empty());
    ASSERT_TRUE(session.GetGroundObjects().empty());
    ASSERT_EQ(P::SaveTransaction::active_for(save), nullptr);
    ASSERT_TRUE(ReconcileMetadata(session, save));
    EXPECT_TRUE(fs::exists(ground_path));
    EXPECT_TRUE(saved_seen.empty());
    EXPECT_EQ(session.CaptureGroundNotifications().canonical_bytes(), notifications);

    EnableSnapshotSession(loaded, root, jobs);
    ASSERT_TRUE(loaded.LoadWorld(session.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_TRUE(loaded.GetGroundObjects().empty());
    EXPECT_EQ(loaded.CaptureGroundNotifications().canonical_bytes(), notifications);
    GroundEvents seen;
    CollectGroundEvents(loaded, seen);
    const GroundEvents expected{
        {"ground.despawned", "second"}, {"ground.spawned", "first"}, {"ground.spawned", "third"}};
    ASSERT_NO_FATAL_FAILURE(TickOnce(loaded));
    EXPECT_EQ(seen, expected);
    EXPECT_TRUE(loaded.CaptureGroundNotifications().events.empty());
    ASSERT_NO_FATAL_FAILURE(TickOnce(loaded));
    EXPECT_EQ(seen, expected);
}

TEST(GameSessionHeadlessWorldTest, EmptyGroundMetadataSaveRemovesExistingFile) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, loaded;
    ASSERT_NO_FATAL_FAILURE(CreateGroundWorld(session, root, jobs, "Ground metadata removal"));
    ASSERT_EQ(SpawnGround(session, 5200, -1500), 1u);
    ASSERT_TRUE(session.SaveWorld());
    const auto save = session.GetWorldSaveDir();
    const auto ground_path = P::WorldSaveService::ground_objects_path(save);
    ASSERT_TRUE(fs::exists(ground_path));
    ASSERT_FALSE(session.GetGroundObjects().empty());
    ASSERT_FALSE(session.CaptureGroundNotifications().events.empty());

    const GroundObjectStore objects;
    const GroundNotifications notifications;
    ASSERT_EQ(P::SaveTransaction::active_for(save), nullptr);
    ASSERT_TRUE(P::WorldSaveService::save_metadata_active_regions_and_ground_objects(
        ReadClockTestFile(save / "world_info.json"),
        session.GetActiveRegionLedger(),
        objects,
        notifications,
        save));
    EXPECT_FALSE(fs::exists(ground_path));
    ASSERT_TRUE(P::WorldSaveService::validate_save(save));

    EnableSnapshotSession(loaded, root, jobs);
    ASSERT_TRUE(loaded.LoadWorld(session.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_TRUE(loaded.GetGroundObjects().empty());
    EXPECT_TRUE(loaded.CaptureGroundNotifications().events.empty());
    EXPECT_EQ(loaded.CaptureGroundNotifications().next_sequence, 0u);
}

} // namespace Luminumbra::world

TEST(GameSessionHeadlessWorldTest, ReloadedSessionRestoresGroundObjectsAndContinuesIds) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, loaded;
    CreateGroundWorld(owner, root, jobs, "Ground reload");
    ASSERT_EQ(SpawnGround(owner, 5200, -1500), 1u);
    ASSERT_EQ(SpawnGround(owner, 5300, -1500), 2u);
    ASSERT_EQ(SpawnGround(owner, 5400, -1500), 3u);
    ASSERT_TRUE(owner.SaveWorld());
    EnableSnapshotSession(loaded, root, jobs);
    ASSERT_TRUE(loaded.LoadWorld(owner.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_EQ(loaded.GetGroundObjects().serialize(), owner.GetGroundObjects().serialize());
    EXPECT_EQ(loaded.GetGroundObjects().size(), 3u);
    EXPECT_EQ(SpawnGround(loaded, 5500, -1500), 4u);
}

TEST(GameSessionHeadlessWorldTest, ResumedSessionMatchesUninterruptedGroundObjectsAndHash) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original, uninterrupted, loaded;
    CreateGroundWorld(original, root, jobs, "Ground resume");
    CreateGroundWorld(uninterrupted, root, jobs, "Ground twin");
    for (auto* s : {&original, &uninterrupted}) {
        ASSERT_EQ(SpawnGround(*s, 5200, -1500), 1u);
        ASSERT_EQ(SpawnGround(*s, -600000, 900000), 2u);
    }
    for (int i = 0; i < 3; ++i) {
        TickOnce(original);
        TickOnce(uninterrupted);
    }
    ASSERT_TRUE(original.SaveWorldState());
    EnableSnapshotSession(loaded, root, jobs);
    ASSERT_TRUE(loaded.LoadWorld(original.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    for (int i = 0; i < 5; ++i) {
        TickOnce(uninterrupted);
        TickOnce(loaded);
        EXPECT_EQ(loaded.GetRegionSchedule(), uninterrupted.GetRegionSchedule());
        EXPECT_EQ(loaded.GetGroundObjects().objects(), uninterrupted.GetGroundObjects().objects());
        EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(uninterrupted));
    }
    EXPECT_EQ(loaded.GetGroundObjects().objects().at(1).active_age, 8u);
}

TEST(GameSessionHeadlessWorldTest, CorruptGroundObjectsFileRefusesToOpen) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, control, victim;
    CreateGroundWorld(owner, root, jobs, "Ground corrupt");
    ASSERT_EQ(SpawnGround(owner, 5200, -1500), 1u);
    ASSERT_TRUE(owner.SaveWorldState());
    const auto save = owner.GetWorldSaveDir();
    EnableSnapshotSession(control, root, jobs);
    ASSERT_TRUE(control.LoadWorld(owner.GetMetadata().worldId)) << control.GetWorldOpenError();
    std::ofstream(P::WorldSaveService::ground_objects_path(save), std::ios::binary) << "not json\n";
    EnableSnapshotSession(victim, root, jobs);
    EXPECT_FALSE(victim.LoadWorld(owner.GetMetadata().worldId));
    EXPECT_FALSE(victim.GetWorldOpenError().empty());
    EXPECT_TRUE(victim.GetGroundObjects().empty());
}

TEST(GameSessionHeadlessWorldTest, BetweenTickGroundSpawnSaveRestoresDeliveryTimingAndWork) {
    for (const bool full_save : {false, true}) {
        SCOPED_TRACE(full_save);
        const HeadlessRoot root;
        JobSystem jobs;
        jobs.startup(1);
        GameSession original, uninterrupted, loaded;
        CreateGroundWorld(original, root, jobs, "Ground pending save");
        CreateGroundWorld(uninterrupted, root, jobs, "Ground pending twin");
        GroundEvents resumed_events, uninterrupted_events;
        CollectGroundEvents(original, resumed_events);
        CollectGroundEvents(uninterrupted, uninterrupted_events);
        std::vector<std::string> resumed_order, uninterrupted_order;
        const auto observe_work = [](GameSession& session, std::vector<std::string>& order) {
            session.GetSimulationEventBus().subscribe([&session, &order](const auto& event) {
                order.push_back(luminumbra::simulation::describe_event_order({event}).front());
                session.RecordActiveRegionWork({kGroundRegion, 0, 2, 0, 0});
            });
        };
        observe_work(original, resumed_order);
        observe_work(uninterrupted, uninterrupted_order);
        for (auto* session : {&original, &uninterrupted}) {
            ASSERT_EQ(SpawnGround(*session, 5200, -1500), 1u);
            TickOnce(*session);
            ASSERT_EQ(SpawnGround(*session, 5300, -1500, 2), 2u);
            ASSERT_EQ(SpawnGround(*session, 5400, -1500), 3u);
        }
        ASSERT_EQ(resumed_events.size(), 1u);
        ASSERT_TRUE(full_save ? original.SaveWorldState() : original.SaveWorld());
        // Saving preserves the next tick's consumer work and notifications.
        EXPECT_EQ(CountTopic(resumed_events, "ground.spawned"), 1u);
        EXPECT_EQ(original.GetSimulationEventBus().pending_count(), 2u);
        EnableSnapshotSession(loaded, root, jobs);
        ASSERT_TRUE(loaded.LoadWorld(original.GetMetadata().worldId)) << loaded.GetWorldOpenError();
        CollectGroundEvents(loaded, resumed_events);
        observe_work(loaded, resumed_order);
        EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(original));
        for (int i = 0; i < 3; ++i) {
            TickOnce(uninterrupted);
            TickOnce(loaded);
            EXPECT_EQ(resumed_events, uninterrupted_events);
            EXPECT_EQ(resumed_order, uninterrupted_order);
            EXPECT_EQ(loaded.GetActiveRegionLedger().completed_work(),
                      uninterrupted.GetActiveRegionLedger().completed_work());
            EXPECT_EQ(loaded.GetRegionSchedule(), uninterrupted.GetRegionSchedule());
            EXPECT_EQ(loaded.GetGroundObjects().objects(),
                      uninterrupted.GetGroundObjects().objects());
            EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(uninterrupted));
        }
        EXPECT_EQ(CountTopic(resumed_events, "ground.despawned"), 1u);
    }
}

TEST(GameSessionHeadlessWorldTest, SavingPendingEventsDoesNotInvokeConsumers) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, loaded;
    CreateGroundWorld(session, root, jobs, "Ground save delivery");
    ASSERT_EQ(SpawnGround(session, 5200, -1500), 1u);
    int calls = 0;
    session.GetSimulationEventBus().subscribe([&](const auto&) {
        ++calls;
        EXPECT_FALSE(session.IsSimulationTickBoundary());
        EXPECT_FALSE(session.SaveWorld());
        EXPECT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 0u);
    });
    session.GetSimulationEventBus().publish(10, "script.trigger", "pending");
    session.GetSimulationEventBus().publish(3, "ground.despawned", "future", -1);
    ASSERT_TRUE(session.SaveWorld());
    EXPECT_EQ(calls, 0);
    EXPECT_TRUE(session.IsSimulationTickBoundary());
    EXPECT_EQ(session.GetSimulationTickCount(), 0u);
    EXPECT_EQ(session.GetSimulationEventBus().pending_count(), 3u);
    EnableSnapshotSession(loaded, root, jobs);
    ASSERT_TRUE(loaded.LoadWorld(session.GetMetadata().worldId));
    const auto pending = loaded.GetSimulationEventBus().pending_events();
    ASSERT_EQ(pending.size(), 2u);
    EXPECT_EQ(pending.front().topic, "ground.spawned");
    EXPECT_EQ(pending.front().tick, 0u);
    EXPECT_EQ(pending.back().topic, "ground.despawned");
    EXPECT_EQ(pending.back().tick, 3u);
    EXPECT_EQ(pending.back().lane, -1);
    EXPECT_EQ(pending.back().sequence, 2u);
    GroundEvents resumed;
    CollectGroundEvents(loaded, resumed);
    TickOnce(session);
    EXPECT_EQ(calls, 1);
    TickOnce(loaded);
    EXPECT_EQ(CountTopic(resumed, "ground.despawned"), 0u);
    TickOnce(loaded);
    EXPECT_EQ(CountTopic(resumed, "ground.despawned"), 0u);
    TickOnce(loaded);
    EXPECT_EQ(CountTopic(resumed, "ground.despawned"), 1u);
}

TEST(GameSessionHeadlessWorldTest, RefusedSavePreservesPendingEventsWorkAndHash) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    CreateGroundWorld(session, root, jobs, "Ground save refusal");
    ASSERT_EQ(SpawnGround(session, 5200, -1500), 1u);
    int calls = 0;
    session.GetSimulationEventBus().subscribe([&](const auto&) {
        ++calls;
        session.RecordActiveRegionWork({kGroundRegion, 0, 2, 0, 0});
    });
    const auto index = P::snapshot_index_path(session.GetWorldSaveDir());
    const auto before = ReadClockTestFile(index);
    const auto hash = SessionWorldHash(session);
    const auto ledger = session.GetActiveRegionLedger().canonical_bytes();
    const auto pending = luminumbra::simulation::describe_event_order(
        session.GetSimulationEventBus().pending_events());
    P::WorldSaveService::set_before_active_regions_replace_for_testing([] { return false; });
    const bool saved = session.SaveWorld();
    P::WorldSaveService::set_before_active_regions_replace_for_testing(nullptr);
    EXPECT_FALSE(saved);
    EXPECT_EQ(calls, 0);
    EXPECT_EQ(ReadClockTestFile(index), before);
    EXPECT_EQ(SessionWorldHash(session), hash);
    EXPECT_EQ(session.GetActiveRegionLedger().canonical_bytes(), ledger);
    EXPECT_EQ(luminumbra::simulation::describe_event_order(
                  session.GetSimulationEventBus().pending_events()),
              pending);
    EXPECT_TRUE(session.IsSimulationTickBoundary());
    EXPECT_EQ(session.GetSimulationTickCount(), 0u);
    ASSERT_TRUE(session.SaveWorld());
    EXPECT_EQ(calls, 0);
    TickOnce(session);
    EXPECT_EQ(calls, 1);
}

TEST(GameSessionHeadlessWorldTest, FaultedSessionSavesWithFutureNotificationsWithoutDelivery) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, loaded;
    CreateGroundWorld(session, root, jobs, "Ground fault save");
    ASSERT_EQ(SpawnGround(session, 5200, -1500), 1u);
    session.GetSimulationEventBus().publish(100, "ground.spawned", "future");
    session.GetSimulationEventBus().publish(100, "script.trigger", "future");
    int calls = 0;
    session.GetSimulationEventBus().subscribe([&](const auto&) {
        ++calls;
        throw std::runtime_error("injected consumer callback fault");
    });
    TickOnce(session);
    ASSERT_TRUE(session.GetRegionSchedulerFault().has_value());
    const auto pending = session.GetSimulationEventBus().pending_count();
    EXPECT_GE(pending, 2u);
    ASSERT_TRUE(session.SaveWorldState());
    ASSERT_TRUE(session.SaveWorld());
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(session.GetSimulationEventBus().pending_count(), pending);
    EnableSnapshotSession(loaded, root, jobs);
    ASSERT_TRUE(loaded.LoadWorld(session.GetMetadata().worldId));
    EXPECT_EQ(loaded.GetSimulationEventBus().pending_count(), 0u);
    EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(session));
}

TEST(GameSessionHeadlessWorldTest, GroundObjectsChangeTheWorldHash) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession bare, with;
    CreateGroundWorld(bare, root, jobs, "Ground hash");
    CreateGroundWorld(with, root, jobs, "Ground hash");
    const auto before = SessionWorldHash(with);
    EXPECT_EQ(SessionWorldHash(bare), before);
    ASSERT_EQ(SpawnGround(with, 5200, -1500), 1u);
    EXPECT_NE(SessionWorldHash(with), before);
}

TEST(GameSessionHeadlessWorldTest, GroundNotificationHashIncludesEveryDurableField) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    CreateGroundWorld(session, root, jobs, "Ground notification hash");
    const auto empty_hash = SessionWorldHash(session);
    EXPECT_TRUE(session.CaptureGroundNotifications().canonical_bytes().empty());
    session.GetSimulationEventBus().publish(4, "ground.spawned", "first", -1);
    session.GetSimulationEventBus().publish(3, "ground.despawned", "second", 2);
    auto notifications = session.CaptureGroundNotifications();
    notifications.next_sequence = 9;
    session.GetSimulationEventBus().restore_pending(notifications.events,
                                                    notifications.next_sequence);
    const auto hash = SessionWorldHash(session);
    ASSERT_NE(hash, empty_hash);
    ASSERT_TRUE(session.GetGroundObjects().empty());
    const std::array<void (*)(Luminumbra::world::GroundNotifications&), 8> mutations{{
        [](auto& n) { ++n.next_sequence; },
        [](auto& n) { ++n.events.front().tick; },
        [](auto& n) { ++n.events.front().lane; },
        [](auto& n) { ++n.events.back().sequence; },
        [](auto& n) { n.events.front().topic = "ground.despawned"; },
        [](auto& n) { n.events.front().payload += "x"; },
        [](auto& n) { n.events.pop_back(); },
        [](auto& n) { n.events.clear(); },
    }};
    for (const auto mutate : mutations) {
        auto changed = notifications;
        mutate(changed);
        session.GetSimulationEventBus().restore_pending(changed.events, changed.next_sequence);
        EXPECT_NE(SessionWorldHash(session), hash);
    }
    session.GetSimulationEventBus().restore_pending({}, notifications.next_sequence);
    EXPECT_NE(SessionWorldHash(session), empty_hash);
    auto reversed = notifications.events;
    std::reverse(reversed.begin(), reversed.end());
    session.GetSimulationEventBus().restore_pending(reversed, notifications.next_sequence);
    EXPECT_EQ(SessionWorldHash(session), hash);
    ASSERT_TRUE(session.SaveWorldState());
    GameSession loaded;
    EnableSnapshotSession(loaded, root, jobs);
    ASSERT_TRUE(loaded.LoadWorld(session.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_EQ(SessionWorldHash(loaded), hash);
    EXPECT_EQ(loaded.CaptureGroundNotifications().canonical_bytes(),
              notifications.canonical_bytes());
}

TEST(GameSessionHeadlessWorldTest, FlagOnIndexedSaveWithGroundObjectsReloadsThroughSnapshot) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, loaded;
    CreateGroundWorld(owner, root, jobs, "Ground snapshot");
    ASSERT_EQ(SpawnGround(owner, 5200, -1500), 1u);
    ASSERT_EQ(SpawnGround(owner, 5300, -1500), 2u);
    ASSERT_TRUE(owner.SaveWorldState());
    const auto save = owner.GetWorldSaveDir();
    ASSERT_TRUE(P::snapshot_index_exists(save));
    P::SnapshotIndex index;
    ASSERT_TRUE(P::read_snapshot_index(save, index, nullptr));
    EXPECT_TRUE(std::any_of(
        index.members.begin(), index.members.end(), [](const P::SnapshotMember& member) {
            return member.path == "chunks/region/ground-objects.json" && member.present;
        }));
    EnableSnapshotSession(loaded, root, jobs);
    ASSERT_TRUE(loaded.LoadWorld(owner.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_EQ(loaded.GetGroundObjects().size(), 2u);
}

TEST(GameSessionHeadlessWorldTest, GroundSpawnQuantizesAndUsesProceduralHeightBeforePublishing) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    CreateGroundWorld(session, root, jobs, "Procedural ground height");
    auto* world = session.GetWorldSystem();
    const float height = world->GetTerrainHeightAt(8, 8);
    const auto expected = std::llround(static_cast<double>(height) * 1000.0);
    ASSERT_TRUE(world->adopt_streamed_chunk(GroundSurfaceChunk({0, 0, 0}, 4.0f)));
    GroundEvents seen;
    CollectGroundEvents(session, seen);
    const auto id =
        session.SpawnGroundObject(kGroundType, Luminumbra::Vec3{8, height + 8, 8}, 0, 0);
    ASSERT_NE(id, 0u);
    const auto* landed = session.GetGroundObjects().find(id);
    ASSERT_NE(landed, nullptr);
    EXPECT_TRUE(landed->settled());
    EXPECT_EQ(landed->y_mm, expected);
    TickOnce(session);
    EXPECT_EQ(FirstPayload(seen, "ground.spawned"),
              std::to_string(id) + ",7,8000," + std::to_string(expected) + ",8000");
    const auto contact =
        session.SpawnGroundObject(kGroundType, Luminumbra::Vec3{8, height + 1, 8}, 0, 0);
    ASSERT_NE(contact, 0u);
    EXPECT_EQ(session.GetGroundObjects().objects().at(contact).y_mm, expected);
    const auto absent_height = world->GetTerrainHeightAt(320, 320);
    const auto absent =
        session.SpawnGroundObject(kGroundType, Luminumbra::Vec3{320, absent_height + 8, 320}, 0, 0);
    ASSERT_NE(absent, 0u);
    EXPECT_TRUE(session.GetGroundObjects().objects().at(absent).settled());
    EXPECT_EQ(session.GetGroundObjects().objects().at(absent).y_mm,
              std::llround(static_cast<double>(absent_height) * 1000.0));
    ASSERT_GT(absent_height, 0);
    const auto rounded =
        session.SpawnGroundObject(kGroundType, Luminumbra::Vec3{320.0005f, -0.0015f, 320}, 0, 0);
    ASSERT_NE(rounded, 0u);
    const auto& quantized = session.GetGroundObjects().objects().at(rounded);
    // -0.0015f is slightly below the half-millimetre tie in binary32.
    EXPECT_EQ(quantized.x_mm, 320000);
    EXPECT_EQ(quantized.y_mm, -2);
    EXPECT_FALSE(quantized.settled());
    for (const auto position : {Luminumbra::Vec3{16777216, 0, 0},
                                Luminumbra::Vec3{0, -1048576, 0},
                                Luminumbra::Vec3{0, 0, -16777216},
                                Luminumbra::Vec3{std::numeric_limits<float>::quiet_NaN(), 0, 0},
                                Luminumbra::Vec3{0, std::numeric_limits<float>::infinity(), 0}}) {
        EXPECT_EQ(session.SpawnGroundObject(kGroundType, position, 0, 0), 0u);
    }
}

TEST(GameSessionHeadlessWorldTest, GroundSpawnIgnoresGeneratedCaveFloorsAndStructures) {
    for (const bool cave : {false, true}) {
        SCOPED_TRACE(cave);
        const HeadlessRoot root;
        JobSystem jobs;
        jobs.startup(1);
        GameSession session;
        CreateGroundWorld(session, root, jobs, "Generated ground height");
        auto chunk = GroundSurfaceChunk({0, 0, 0}, cave ? 2.5f : 12.5f);
        if (cave) {
            constexpr int nx = Luminumbra::CHUNK_SIZE_X + 1;
            constexpr int ny = Luminumbra::CHUNK_SIZE_Y + 1;
            constexpr int nz = Luminumbra::CHUNK_SIZE_Z + 1;
            for (int z = 0; z < nz; ++z) {
                for (int y = 0; y < ny; ++y) {
                    for (int x = 0; x < nx; ++x) {
                        auto& density =
                            chunk->sdf_data[static_cast<std::size_t>(x + nx * (y + ny * z))];
                        density = std::min(density, 7.5f - static_cast<float>(y));
                    }
                }
            }
        }
        chunk->mark_sdf_generated_current_params();
        ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
        ASSERT_EQ(chunk->sdf_provenance(), Luminumbra::ChunkSdfProvenance::GeneratedCurrentParams);
        const float procedural = session.GetWorldSystem()->GetTerrainHeightAt(8, 8);
        const float start = std::max(procedural, 12.5f) + 2;
        const auto id = session.SpawnGroundObject(kGroundType, Luminumbra::Vec3{8, start, 8}, 0, 0);
        ASSERT_NE(id, 0u);
        const auto& landed = session.GetGroundObjects().objects().at(id);
        EXPECT_TRUE(landed.settled());
        EXPECT_EQ(landed.y_mm, std::llround(static_cast<double>(procedural) * 1000.0));
    }
}

TEST(GameSessionHeadlessWorldTest, DespawnedGroundWorkSurvivesBoundarySaveAndReopen) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, loaded;
    CreateGroundWorld(owner, root, jobs, "Completed despawn work");
    EnableSnapshotSession(loaded, root, jobs);
    ASSERT_EQ(SpawnGround(owner, 5200, -1500, 1), 1u);
    TickOnce(owner);
    EXPECT_TRUE(owner.GetGroundObjects().empty());
    EXPECT_EQ(owner.GetActiveRegionLedger().completed_work().at(kGroundRegion), 1u);
    EXPECT_TRUE(owner.IsSimulationTickBoundary());
    ASSERT_TRUE(owner.SaveWorldState());
    ASSERT_TRUE(owner.SaveWorld());
    ASSERT_TRUE(loaded.LoadWorld(owner.GetMetadata().worldId));
    EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(owner));
    TickOnce(owner);
    TickOnce(loaded);
    EXPECT_EQ(owner.GetRegionSchedule().work, 1u);
    EXPECT_EQ(loaded.GetRegionSchedule(), owner.GetRegionSchedule());
    EXPECT_TRUE(owner.GetActiveRegionLedger().completed_work().empty());
    EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(owner));
    TickOnce(owner);
    EXPECT_EQ(owner.GetRegionSchedule().work, 0u);
}

TEST(GameSessionHeadlessWorldTest, EventConsumerFaultStopsDeliveryAndClearsCompletedWork) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner;
    CreateGroundWorld(owner, root, jobs, "Consumer event fault");
    ASSERT_EQ(SpawnGround(owner, 5200, -1500, 1), 1u);
    int later_calls = 0;
    owner.GetSimulationEventBus().subscribe(
        [](const auto&) { throw std::runtime_error("injected consumer callback fault"); });
    owner.GetSimulationEventBus().subscribe([&](const auto&) { ++later_calls; });
    TickOnce(owner);
    EXPECT_EQ(later_calls, 0);
    EXPECT_TRUE(owner.GetRegionSchedulerFault().has_value());
    EXPECT_EQ(owner.GetActiveRegionLedger().tick(), 1u);
    EXPECT_TRUE(owner.GetActiveRegionLedger().completed_work().empty());
    EXPECT_TRUE(owner.IsSimulationTickBoundary());
    EXPECT_EQ(owner.TickSimulation(owner.GetSimulationClock().fixed_dt()), 0u);
}

TEST(GameSessionHeadlessWorldTest, CompletedWorkFaultAfterStormEpochRestoresEverySubHash) {
    for (const std::uint64_t start : {46u, 45u}) {
        ASSERT_NO_FATAL_FAILURE(RunSchedulerFaultCase(46, start));
    }
}

TEST(GameSessionHeadlessWorldTest,
     GroundHeightIsIdenticalWithoutResidencyWithFullCoarseAndEditedTerrain) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    CreateGroundWorld(session, root, jobs, "Ground residency independence");
    auto* world = session.GetWorldSystem();
    constexpr float x = 328.25f, z = 328.5f;
    const float height = world->GetTerrainHeightAt(x, z);
    const auto expected = std::llround(static_cast<double>(height) * 1000.0);
    const Luminumbra::IVec3 coords{
        20, static_cast<int>(std::floor(height / Luminumbra::CHUNK_SIZE_Y)), 20};
    ASSERT_EQ(world->find_streamed_chunk(coords), nullptr);
    auto chunk = GroundSurfaceChunk(coords, height - 2);
    for (int state = 0; state < 4; ++state) {
        SCOPED_TRACE(state);
        if (state == 1) {
            chunk->mark_sdf_generated_current_params();
            ASSERT_TRUE(world->adopt_streamed_chunk(chunk));
        } else if (state == 2) {
            chunk->current_lod.store(2);
            chunk->sdf_data.clear();
            chunk->heightmap_data.assign(
                (Luminumbra::CHUNK_SIZE_X + 1) * (Luminumbra::CHUNK_SIZE_Z + 1), height - 3);
            chunk->mark_sdf_generated_current_params();
        } else if (state == 3) {
            chunk->current_lod.store(0);
            chunk->sdf_data = GroundSurfaceChunk(coords, height + 2)->sdf_data;
            chunk->mark_voxel_data_dirty();
            ASSERT_TRUE(world->adopt_streamed_chunk(
                GroundSurfaceChunk({coords.x + 1, coords.y, coords.z}, height + 4)));
        }
        const auto id =
            session.SpawnGroundObject(kGroundType, Luminumbra::Vec3{x, height + 8, z}, 0, 0);
        ASSERT_NE(id, 0u);
        const auto& object = session.GetGroundObjects().objects().at(id);
        EXPECT_TRUE(object.settled());
        EXPECT_EQ(object.y_mm, expected);
    }
    ASSERT_NO_FATAL_FAILURE(RestoreUnsettledGroundObjects(session));
    for (int i = 0; i < 3; ++i) {
        TickOnce(session);
        for (const auto& [id, object] : session.GetGroundObjects().objects()) {
            EXPECT_TRUE(object.settled());
            EXPECT_EQ(object.y_mm, expected);
            EXPECT_EQ(object.active_age, static_cast<std::uint64_t>(i + 1));
        }
    }
}

TEST(GameSessionHeadlessWorldTest, GroundIntegerLimitsSettleAtSpawnAndAfterReload) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    CreateGroundWorld(session, root, jobs, "Ground integer limits");
    using Luminumbra::world::GroundObjectStore;
    constexpr auto edge = GroundObjectStore::kMaxCoordinateMm - 1;
    constexpr auto top = GroundObjectStore::kMaxVerticalMm - 1;
    struct Position {
        std::int64_t x_mm, z_mm;
        float sample_x, sample_z;
    };
    const std::array<Position, 5> positions{{{5200, -1500, 5.2f, -1.5f},
                                             {edge, edge, 16777215.0f, 16777215.0f},
                                             {-edge, -edge, -16777215.0f, -16777215.0f},
                                             {edge, -edge, 16777215.0f, -16777215.0f},
                                             {-edge, edge, -16777215.0f, 16777215.0f}}};
    GroundObjectStore unsettled;
    std::map<std::uint64_t, std::int64_t> expected;
    for (const auto& [x, z, sample_x, sample_z] : positions) {
        const float height = session.GetWorldSystem()->GetTerrainHeightAt(sample_x, sample_z);
        ASSERT_TRUE(std::isfinite(height));
        const auto id = session.SpawnGroundObject(kGroundType, x, top, z, 0, 0);
        ASSERT_NE(id, 0u);
        const auto& object = session.GetGroundObjects().objects().at(id);
        expected.emplace(id, std::llround(static_cast<double>(height) * 1000.0));
        EXPECT_TRUE(object.settled());
        EXPECT_EQ(object.y_mm, expected.at(id));
        auto waiting = object;
        waiting.flags = 0;
        waiting.y_mm = top;
        ASSERT_TRUE(unsettled.insert(waiting));
        session.PinActiveRegion(GroundObjectStore::region_of(object), true);
    }
    ASSERT_TRUE(session.SaveWorldState());
    ASSERT_TRUE(P::WorldSaveService::save_ground_objects(
        unsettled, session.GetWorldSaveDir(), nullptr, session.CaptureGroundNotifications()));
    ASSERT_TRUE(session.LoadWorldState()) << session.GetWorldOpenError();
    ASSERT_EQ(session.GetGroundObjects().size(), positions.size());
    for (const auto& [id, object] : session.GetGroundObjects().objects()) {
        EXPECT_FALSE(object.settled());
        EXPECT_EQ(object.y_mm, top);
    }
    for (int i = 0; i < 3; ++i) {
        TickOnce(session);
        ASSERT_FALSE(session.GetRegionSchedulerFault().has_value());
        for (const auto& [id, object] : session.GetGroundObjects().objects()) {
            EXPECT_TRUE(object.settled());
            EXPECT_EQ(object.y_mm, expected.at(id));
            EXPECT_EQ(object.active_age, static_cast<std::uint32_t>(i + 1));
        }
    }
    // The excluded integer bounds still refuse before allocating an id.
    EXPECT_EQ(session.SpawnGroundObject(kGroundType, 0, top + 1, 0, 0, 0), 0u);
    EXPECT_EQ(session.SpawnGroundObject(kGroundType, edge + 1, top, 0, 0, 0), 0u);
    EXPECT_EQ(session.SpawnGroundObject(kGroundType, 0, top, -edge - 1, 0, 0), 0u);
}

TEST(GameSessionHeadlessWorldTest, EmptyGroundStoreSkipsAdvancementWork) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    CreateGroundWorld(session, root, jobs, "Empty ground advancement");
    session.PinActiveRegion(kGroundRegion, true);
    const auto before = session.GetGroundObjectAdvanceCount();
    for (int i = 0; i < 3; ++i) {
        TickOnce(session);
        ASSERT_FALSE(session.GetRegionSchedule().due.empty());
        EXPECT_EQ(session.GetGroundObjectAdvanceCount(), before);
    }
    ASSERT_NE(SpawnGround(session, 5200, -1500, 1), 0u);
    TickOnce(session);
    ASSERT_TRUE(session.GetGroundObjects().empty());
    EXPECT_EQ(session.GetGroundObjectAdvanceCount(), before + 1);
    for (int i = 0; i < 3; ++i) {
        TickOnce(session);
        ASSERT_FALSE(session.GetRegionSchedule().due.empty());
        EXPECT_EQ(session.GetGroundObjectAdvanceCount(), before + 1);
    }
}

TEST(GameSessionHeadlessWorldTest,
     UnsettledDistantGroundMatchesPristineResidentAndReopenedContinuation) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original, reopened;
    CreateGroundWorld(original, root, jobs, "Distant ground continuation");
    auto* world = original.GetWorldSystem();
    const float height = world->GetTerrainHeightAt(5208.25f, -1527.5f);
    const auto expected = std::llround(static_cast<double>(height) * 1000.0);
    const Luminumbra::IVec3 coords{
        325, static_cast<int>(std::floor(height / Luminumbra::CHUNK_SIZE_Y)), -96};
    const Luminumbra::Vec3 position{5208.25f, height + 8, -1527.5f};
    ASSERT_EQ(world->find_streamed_chunk(coords), nullptr);
    const auto id = original.SpawnGroundObject(kGroundType, position, 0, 0);
    ASSERT_NE(id, 0u);
    const float short_height = world->GetTerrainHeightAt(5212.25f, -1527.5f);
    const auto short_lived = original.SpawnGroundObject(
        kGroundType, Luminumbra::Vec3{5212.25f, short_height + 8, -1527.5f}, 1, 0);
    ASSERT_NE(short_lived, 0u);
    original.PinActiveRegion(Luminumbra::world::GroundObjectStore::region_of(
                                 original.GetGroundObjects().objects().at(id)),
                             true);
    ASSERT_NO_FATAL_FAILURE(RestoreUnsettledGroundObjects(original));
    ASSERT_FALSE(original.GetGroundObjects().objects().at(id).settled());
    auto pristine = GroundSurfaceChunk(coords, height - 2);
    pristine->mark_sdf_generated_current_params();
    ASSERT_TRUE(world->adopt_streamed_chunk(pristine));
    ASSERT_TRUE(original.SaveWorldState());
    ASSERT_FALSE(original.GetGroundObjects().objects().at(id).settled());
    ASSERT_NE(world->find_streamed_chunk(coords), nullptr);

    EnableSnapshotSession(reopened, root, jobs);
    ASSERT_TRUE(reopened.LoadWorld(original.GetMetadata().worldId)) << reopened.GetWorldOpenError();
    auto* restored = reopened.GetWorldSystem();
    ASSERT_EQ(restored->find_streamed_chunk(coords), nullptr);
    const auto parked = restored->snapshot_parked_chunks();
    ASSERT_TRUE(std::none_of(parked.begin(), parked.end(), [&coords](const auto& chunk) {
        return chunk->get_coords() == coords;
    }));
    const auto durable = restored->durable_chunk_inventory();
    ASSERT_EQ(std::find(durable.begin(), durable.end(), coords), durable.end());
    EXPECT_EQ(reopened.GetGroundObjects().serialize(), original.GetGroundObjects().serialize());
    EXPECT_EQ(SessionWorldHash(reopened), SessionWorldHash(original));
    GroundEvents original_events, reopened_events;
    CollectGroundEvents(original, original_events);
    CollectGroundEvents(reopened, reopened_events);
    for (int i = 0; i < 5; ++i) {
        TickOnce(original);
        TickOnce(reopened);
        ASSERT_FALSE(original.GetRegionSchedulerFault().has_value());
        ASSERT_FALSE(reopened.GetRegionSchedulerFault().has_value());
        EXPECT_EQ(reopened.GetRegionSchedule(), original.GetRegionSchedule());
        EXPECT_EQ(reopened.GetGroundObjects().serialize(), original.GetGroundObjects().serialize());
        EXPECT_EQ(SessionWorldHash(reopened), SessionWorldHash(original));
        EXPECT_EQ(reopened_events, original_events);
        for (const auto* session : {&original, &reopened}) {
            const auto& object = session->GetGroundObjects().objects().at(id);
            EXPECT_TRUE(object.settled());
            EXPECT_EQ(object.y_mm, expected);
            EXPECT_EQ(object.active_age, static_cast<std::uint64_t>(i + 1));
            EXPECT_EQ(session->GetGroundObjects().find(short_lived), nullptr);
        }
        EXPECT_EQ(restored->find_streamed_chunk(coords), nullptr);
    }
    EXPECT_EQ(FirstPayload(original_events, "ground.despawned"),
              std::to_string(short_lived) + ",7,5212250," +
                  std::to_string(std::llround(static_cast<double>(short_height) * 1000.0)) +
                  ",-1527500");
    const auto original_spawn = original.SpawnGroundObject(kGroundType, position, 0, 0);
    const auto reopened_spawn = reopened.SpawnGroundObject(kGroundType, position, 0, 0);
    ASSERT_NE(original_spawn, 0u);
    ASSERT_EQ(reopened_spawn, original_spawn);
    EXPECT_TRUE(reopened.GetGroundObjects().objects().at(reopened_spawn).settled());
    EXPECT_EQ(reopened.GetGroundObjects().serialize(), original.GetGroundObjects().serialize());
    EXPECT_EQ(SessionWorldHash(reopened), SessionWorldHash(original));
    ASSERT_TRUE(original.SaveWorldState());
    const auto ground_bytes =
        ReadClockTestFile(P::WorldSaveService::ground_objects_path(original.GetWorldSaveDir()));
    ASSERT_TRUE(reopened.SaveWorldState());
    EXPECT_EQ(
        ReadClockTestFile(P::WorldSaveService::ground_objects_path(reopened.GetWorldSaveDir())),
        ground_bytes);
}

TEST(GameSessionHeadlessWorldTest, GroundSettlingNeverReadsCommittedTerrain) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    CreateGroundWorld(session, root, jobs, "Ground without terrain reads");
    auto* world = session.GetWorldSystem();
    const float height = world->GetTerrainHeightAt(5208, -1528);
    const Luminumbra::Vec3 position{5208, height + 8, -1528};
    const auto id = session.SpawnGroundObject(kGroundType, position, 0, 0);
    ASSERT_NE(id, 0u);
    session.PinActiveRegion(Luminumbra::world::GroundObjectStore::region_of(
                                session.GetGroundObjects().objects().at(id)),
                            true);
    ASSERT_NO_FATAL_FAILURE(RestoreUnsettledGroundObjects(session));
    std::size_t reads = 0;
    std::string fault;
    world->remember_durable_chunks({{325, 0, -96}, {325, 1, -96}});
    world->set_durable_chunk_loader(
        [&reads](const auto&) -> std::shared_ptr<Luminumbra::Chunk> {
            ++reads;
            throw std::runtime_error("committed terrain unavailable");
        },
        [&fault](const auto& error) { fault = error; });
    const auto spawned = session.SpawnGroundObject(kGroundType, position, 0, 0);
    ASSERT_NE(spawned, 0u);
    TickOnce(session);
    for (const auto object_id : {id, spawned}) {
        const auto& object = session.GetGroundObjects().objects().at(object_id);
        EXPECT_TRUE(object.settled());
        EXPECT_EQ(object.y_mm, std::llround(static_cast<double>(height) * 1000.0));
        EXPECT_EQ(object.active_age, 1u);
    }
    EXPECT_EQ(reads, 0u);
    EXPECT_TRUE(fault.empty());
    EXPECT_FALSE(session.GetRegionSchedulerFault().has_value());
}

namespace {

using Luminumbra::world::FieldPageState;
constexpr Luminumbra::world::RegionKey kFieldOwner{0, 0};

void PopulateSessionFieldFixture(FieldPageState& fields, std::uint64_t seed) {
    ASSERT_TRUE(fields.activate_region(kFieldOwner, seed, 0));
    ASSERT_TRUE(fields.set_weather_age(kFieldOwner, 7));
    ASSERT_TRUE(fields.set_scent({1, 1}, {0x3ff0000000000000ULL, 0, 0, 0}));
    ASSERT_TRUE(fields.set_soil({1, 1}, 123));
    ASSERT_TRUE(fields.set_irrigation({1, 1}, 456));
    ASSERT_FALSE(fields.empty());
}

void WriteSessionFieldInput(const GameSession& session,
                            const fs::path& directory,
                            const FieldPageState& fields) {
    fs::create_directories(directory);
    auto ledger = session.GetActiveRegionLedger();
    ledger.mark_edited(kFieldOwner, session.GetWorldClock());
    nlohmann::json metadata = {{"container_version", P::WorldSaveService::kContainerVersion}};
    session.GetWorldClock().write_metadata(metadata);
    // Field pages are indexed-envelope members, so the fixture is committed through a transaction.
    P::WorldSaveService::SaveFactsScope facts(directory);
    P::SaveTransaction transaction(directory, 0, {}, &P::WorldSaveService::member_digest);
    std::vector<std::string> errors;
    ASSERT_TRUE(transaction.begin(&errors));
    ASSERT_TRUE(P::WorldSaveService::save_metadata_and_active_regions(
        metadata.dump(), ledger, directory, &errors));
    ASSERT_TRUE(P::WorldSaveService::save_field_pages(fields, directory, &errors));
    ASSERT_TRUE(P::WorldSaveService::finalize_indexed_envelope(directory, &errors));
    ASSERT_TRUE(transaction.commit(&errors));
}

void LoadSessionFieldInput(GameSession& session,
                           const fs::path& directory,
                           const FieldPageState& fields) {
    ASSERT_NO_FATAL_FAILURE(WriteSessionFieldInput(session, directory, fields));
    ASSERT_TRUE(session.LoadWorldStateFrom(directory)) << session.GetWorldOpenError();
    ASSERT_EQ(session.GetFieldPages().serialize(), fields.serialize());
}

void ExpectIndexedSessionFields(const fs::path& directory, const FieldPageState& fields) {
    ASSERT_TRUE(P::WorldSaveService::validate_save(directory));
    P::SnapshotIndex index;
    ASSERT_TRUE(P::read_snapshot_index(directory, index, nullptr));
    const auto member = std::find_if(index.members.begin(), index.members.end(), [](const auto& m) {
        return m.path == FieldPageState::kRelativePath;
    });
    ASSERT_NE(member, index.members.end());
    EXPECT_EQ(member->present, !fields.empty());
    EXPECT_EQ(member->bytes, fields.serialize().size());
    FieldPageState saved;
    ASSERT_TRUE(P::WorldSaveService::load_field_pages(saved, directory));
    EXPECT_EQ(saved.canonical_bytes(), fields.canonical_bytes());
}

void CheckSessionFieldRoundTrip(int save_path) {
    SCOPED_TRACE(save_path);
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession owner, loaded;
    EnableSnapshotSession(owner, root, jobs);
    EnableSnapshotSession(loaded, root, jobs);
    ASSERT_TRUE(owner.CreateWorld("Field roundtrip", "1337", "default"));
    FieldPageState fields;
    ASSERT_NO_FATAL_FAILURE(PopulateSessionFieldFixture(fields, 1337));
    ASSERT_NO_FATAL_FAILURE(LoadSessionFieldInput(owner, root.path() / "field-input", fields));
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3{0, 0, 0});
    chunk->set_state(Luminumbra::ChunkState::Idle);
    chunk->heightmap_data = {3.0f};
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(owner.GetWorldSystem()->adopt_streamed_chunk(chunk));
    auto save = owner.GetWorldSaveDir();
    if (save_path == 0) {
        ASSERT_TRUE(owner.SaveWorld());
    } else if (save_path == 1) {
        ASSERT_TRUE(owner.SaveWorldState());
    } else {
        save = root.path() / "field-export";
        ASSERT_TRUE(owner.SaveWorldStateTo(save));
    }
    ASSERT_NO_FATAL_FAILURE(ExpectIndexedSessionFields(save, fields));
    EXPECT_EQ(chunk->is_voxel_data_dirty(), save_path == 2);
    ASSERT_TRUE(loaded.CreateTransientWorld("Field reader", "1337", "default"));
    ASSERT_TRUE(loaded.LoadWorldStateFrom(save));
    EXPECT_EQ(loaded.GetFieldPages().canonical_bytes(), fields.canonical_bytes());
    EXPECT_EQ(loaded.GetLastLoadedChunkCount(), 1u);
    EXPECT_EQ(loaded.GetActiveRegionLedger().canonical_bytes(),
              owner.GetActiveRegionLedger().canonical_bytes());
    if (save_path != 2) {
        ASSERT_TRUE(loaded.LoadWorld(owner.GetMetadata().worldId));
        EXPECT_EQ(loaded.GetFieldPages().serialize(), fields.serialize());
    }
    ASSERT_TRUE(loaded.CreateTransientWorld("Field reset", "7331", "default"));
    EXPECT_TRUE(loaded.GetFieldPages().empty());
    EXPECT_TRUE(loaded.GetFieldPages().canonical_bytes().empty());
}

void CheckSessionFieldRollback(int transition) {
    SCOPED_TRACE(transition);
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    EnableSnapshotSession(session, root, jobs);
    ASSERT_TRUE(session.CreateWorld("Field rollback", "1337", "default"));
    FieldPageState original, replacement;
    ASSERT_NO_FATAL_FAILURE(PopulateSessionFieldFixture(original, 1337));
    ASSERT_NO_FATAL_FAILURE(PopulateSessionFieldFixture(replacement, 7331));
    if (transition != 0) {
        ASSERT_NO_FATAL_FAILURE(
            LoadSessionFieldInput(session, root.path() / "original-input", original));
    }
    ASSERT_TRUE(session.SaveWorld());
    const auto save = session.GetWorldSaveDir();
    const auto before = SavedPresetTestDiskBytes(save);
    if (transition == 2)
        replacement.clear();
    ASSERT_NO_FATAL_FAILURE(
        LoadSessionFieldInput(session, root.path() / "replacement-input", replacement));
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3{0, 0, 0});
    chunk->heightmap_data = {9.0f};
    chunk->mark_voxel_data_dirty();
    ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
    static fs::path observed_path;
    static std::string observed_bytes;
    observed_path = P::WorldSaveService::field_pages_path(save);
    observed_bytes.clear();
    P::WorldSaveService::set_before_active_regions_replace_for_testing([] {
        observed_bytes = fs::exists(observed_path) ? ReadClockTestFile(observed_path) : "<absent>";
        return false;
    });
    const bool saved = session.SaveWorld();
    P::WorldSaveService::set_before_active_regions_replace_for_testing(nullptr);
    EXPECT_FALSE(saved);
    EXPECT_EQ(observed_bytes, replacement.empty() ? "<absent>" : replacement.serialize());
    EXPECT_EQ(SavedPresetTestDiskBytes(save), before);
    EXPECT_EQ(session.GetFieldPages().serialize(), replacement.serialize());
    EXPECT_TRUE(chunk->is_voxel_data_dirty());
    EXPECT_FALSE(P::journal_pending(save));
    ASSERT_TRUE(P::WorldSaveService::validate_save(save));
    ASSERT_TRUE(session.SaveWorld());
    EXPECT_FALSE(chunk->is_voxel_data_dirty());
    ASSERT_NO_FATAL_FAILURE(ExpectIndexedSessionFields(save, replacement));
}

void CheckPreparedFieldRefusal(int refusal) {
    SCOPED_TRACE(refusal);
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    EnableSnapshotSession(session, root, jobs);
    ASSERT_TRUE(session.CreateWorld("Prepared fields", "1337", "default"));
    FieldPageState live, candidate;
    ASSERT_NO_FATAL_FAILURE(PopulateSessionFieldFixture(live, 1337));
    ASSERT_NO_FATAL_FAILURE(PopulateSessionFieldFixture(candidate, 7331));
    ASSERT_NO_FATAL_FAILURE(LoadSessionFieldInput(session, root.path() / "live-input", live));
    const auto input = root.path() / "candidate-input";
    ASSERT_NO_FATAL_FAILURE(WriteSessionFieldInput(session, input, candidate));
    std::stop_source stop;
    const auto prepared = GameSession::PrepareWorldState(
        session.CaptureWorldPreparationOptions(), input, stop.get_token());
    ASSERT_TRUE(GameSession::WorldPreparationError(*prepared).empty());
    if (refusal == 0) {
        stop.request_stop();
    } else if (refusal == 1) {
        session.SetAetherStateEnabled(false);
    } else {
        ASSERT_TRUE(session.CreateTransientWorld("New field owner", "1337", "default"));
        ASSERT_NO_FATAL_FAILURE(LoadSessionFieldInput(session, root.path() / "live-input", live));
    }
    const auto ledger = session.GetActiveRegionLedger().canonical_bytes();
    const auto clock = session.GetWorldClock().canonical_bytes();
    const auto revision = session.WorldRevision();
    EXPECT_FALSE(session.AdoptPreparedWorld(prepared));
    EXPECT_EQ(session.GetWorldOpenError(),
              refusal == 0 ? "Saved-world validation cancelled."
                           : "World preparation no longer matches session configuration.");
    EXPECT_EQ(session.GetFieldPages().serialize(), live.serialize());
    EXPECT_EQ(session.GetActiveRegionLedger().canonical_bytes(), ledger);
    EXPECT_EQ(session.GetWorldClock().canonical_bytes(), clock);
    EXPECT_EQ(session.WorldRevision(), revision);
}

void CheckPendingRecoveryKeepsSessionFields(int load_path) {
    SCOPED_TRACE(load_path);
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    EnableSnapshotSession(session, root, jobs);
    ASSERT_TRUE(session.CreateWorld("Pending fields", "1337", "default"));
    FieldPageState fields;
    ASSERT_NO_FATAL_FAILURE(PopulateSessionFieldFixture(fields, 1337));
    ASSERT_NO_FATAL_FAILURE(LoadSessionFieldInput(session, root.path() / "live-input", fields));
    ASSERT_TRUE(session.SaveWorld());
    const auto save = session.GetWorldSaveDir();
    const auto revision = session.WorldRevision();
    const auto clock = session.GetWorldClock().canonical_bytes();
    ASSERT_TRUE(fs::create_directories(P::journal_directory(save)));
    ASSERT_TRUE(P::WorldSaveService::write_file_durably(P::journal_directory(save) / "txn.json",
                                                        "{invalid journal"));
    const auto before = SavedPresetTestDiskBytes(save);
    if (load_path == 0) {
        EXPECT_FALSE(session.LoadWorldStateFrom(save));
    } else if (load_path == 1) {
        EXPECT_FALSE(session.LoadWorld(session.GetMetadata().worldId));
    } else if (load_path == 2) {
        EXPECT_FALSE(session.LoadWorldFrom(session.GetMetadata().worldId, root.path()));
    } else {
        const auto prepared =
            GameSession::PrepareWorldState(session.CaptureWorldPreparationOptions(), save);
        EXPECT_FALSE(session.AdoptPreparedWorld(prepared));
    }
    EXPECT_FALSE(session.GetWorldOpenError().empty());
    EXPECT_EQ(session.GetFieldPages().serialize(), fields.serialize());
    EXPECT_EQ(session.GetWorldClock().canonical_bytes(), clock);
    EXPECT_EQ(session.WorldRevision(), revision);
    EXPECT_EQ(SavedPresetTestDiskBytes(save), before);
    EXPECT_TRUE(P::journal_pending(save));
}

} // namespace

TEST(GameSessionHeadlessWorldTest, SessionFieldPagesSaveLoadAndResetThroughEverySavePath) {
    for (const int save_path : {0, 1, 2}) {
        ASSERT_NO_FATAL_FAILURE(CheckSessionFieldRoundTrip(save_path));
    }
}

TEST(GameSessionHeadlessWorldTest, SessionFieldPagesJoinCompoundRollbackAndRetry) {
    for (const int transition : {0, 1, 2}) {
        ASSERT_NO_FATAL_FAILURE(CheckSessionFieldRollback(transition));
    }
}

TEST(GameSessionHeadlessWorldTest, RefusedPreparedFieldAdoptionPreservesLiveMembers) {
    for (const int refusal : {0, 1, 2}) {
        ASSERT_NO_FATAL_FAILURE(CheckPreparedFieldRefusal(refusal));
    }
}

TEST(GameSessionHeadlessWorldTest, PendingRecoveryRefusalPreservesSessionFieldPages) {
    for (const int load_path : {0, 1, 2, 3}) {
        ASSERT_NO_FATAL_FAILURE(CheckPendingRecoveryKeepsSessionFields(load_path));
    }
}

namespace {

void CheckCapturedFieldAdoptionAndMissingMember() {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    EnableSnapshotSession(session, root, jobs);
    ASSERT_TRUE(session.CreateWorld("Captured fields", "1337", "default"));
    FieldPageState fields;
    ASSERT_NO_FATAL_FAILURE(PopulateSessionFieldFixture(fields, 1337));
    const auto input = root.path() / "field-input";
    ASSERT_NO_FATAL_FAILURE(WriteSessionFieldInput(session, input, fields));
    const auto prepared =
        GameSession::PrepareWorldState(session.CaptureWorldPreparationOptions(), input);
    ASSERT_TRUE(GameSession::WorldPreparationError(*prepared).empty());
    ASSERT_TRUE(P::WorldSaveService::save_field_pages({}, input));
    ASSERT_TRUE(session.AdoptPreparedWorld(prepared));
    EXPECT_EQ(session.GetFieldPages().serialize(), fields.serialize());
    EXPECT_FALSE(session.AdoptPreparedWorld(prepared));
    EXPECT_EQ(session.GetFieldPages().serialize(), fields.serialize());
    ASSERT_TRUE(session.LoadWorldStateFrom(input));
    EXPECT_TRUE(session.GetFieldPages().empty());
    EXPECT_TRUE(session.GetFieldPages().canonical_bytes().empty());
    Luminumbra::Systems::WindFieldSystem wind(1337);
    Luminumbra::Systems::WeatherSystem weather(1337);
    Luminumbra::Systems::AetherFieldSystem aether(1337);
    const auto anchor = session.GetMetadata().spawnPoint;
    weather.RestoreAtTick(session.GetWorldClock().tick(), anchor, wind);
    aether.Update(session.GetWorldClock().tick(), anchor, &wind);
    EXPECT_EQ(session.GetWindFieldSystem()->ComputeWindSubHash(), wind.ComputeWindSubHash());
    EXPECT_EQ(session.GetWeatherSystem()->ComputeWeatherSubHash(), weather.ComputeWeatherSubHash());
    EXPECT_EQ(session.GetAetherFieldSystem()->ComputeAetherSubHash(),
              aether.ComputeAetherSubHash());
}

void CheckEmptySessionFieldCompatibility(bool active_regions) {
    SCOPED_TRACE(active_regions);
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    EnableSnapshotSession(session, root, jobs);
    session.SetActiveRegionsEnabled(active_regions);
    ASSERT_TRUE(session.CreateWorld("Empty fields", "1337", "default"));
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    EXPECT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_TRUE(session.GetFieldPages().empty());
    Luminumbra::Systems::WindFieldSystem wind(1337);
    Luminumbra::Systems::WeatherSystem weather(1337);
    Luminumbra::Systems::AetherFieldSystem aether(1337);
    const auto anchor = session.GetMetadata().spawnPoint;
    if (active_regions) {
        weather.RestoreAtTick(1, anchor, wind);
    } else {
        wind.Update(1, anchor);
        weather.Update(1, anchor, &wind);
    }
    aether.Update(1, anchor, &wind);
    EXPECT_EQ(session.GetWindFieldSystem()->ComputeWindSubHash(), wind.ComputeWindSubHash());
    EXPECT_EQ(session.GetWeatherSystem()->ComputeWeatherSubHash(), weather.ComputeWeatherSubHash());
    EXPECT_EQ(session.GetAetherFieldSystem()->ComputeAetherSubHash(),
              aether.ComputeAetherSubHash());
    EXPECT_TRUE(session.GetActiveRegionLedger().completed_work().empty());
    EXPECT_EQ(session.GetRegionSchedule().work, 0u);
    ASSERT_TRUE(session.SaveWorld());
    const auto save = session.GetWorldSaveDir();
    const auto control = root.path() / "without-field-writer";
    fs::copy(save, control, fs::copy_options::recursive);
    const auto ecology = session.FoldClockIntoEcologyHash("ecology");
    P::RegionDigestInputs inputs;
    inputs.require_index = active_regions;
    const auto digest = P::WorldSaveService::region_simulation_digest(save, kFieldOwner, inputs);
    const auto metadata = ReadClockTestFile(save / "world_info.json");
    // Reproduce the existing payload writers without adding a field member to the transaction.
    if (active_regions) {
        ASSERT_TRUE(Luminumbra::Test::PublishIndexedSave(control, [&] {
            return P::WorldSaveService::save_plant_entities({}, control) &&
                   P::WorldSaveService::save_metadata_active_regions_and_ground_objects(
                       metadata,
                       session.GetActiveRegionLedger(),
                       session.GetGroundObjects(),
                       session.CaptureGroundNotifications(),
                       control);
        }));
    }
    ASSERT_TRUE(session.SaveWorld());
    EXPECT_TRUE(session.GetFieldPages().empty());
    EXPECT_EQ(SavedPresetTestDiskBytes(save), SavedPresetTestDiskBytes(control));
    EXPECT_EQ(session.FoldClockIntoEcologyHash("ecology"), ecology);
    EXPECT_EQ(P::WorldSaveService::region_simulation_digest(save, kFieldOwner, inputs), digest);
    EXPECT_FALSE(fs::exists(P::WorldSaveService::field_pages_path(save)));
    if (active_regions) {
        P::SnapshotIndex index;
        ASSERT_TRUE(P::read_snapshot_index(save, index, nullptr));
        EXPECT_TRUE(
            std::none_of(index.members.begin(), index.members.end(), [](const auto& member) {
                return member.path == FieldPageState::kRelativePath;
            }));
    }
}

} // namespace

TEST(GameSessionHeadlessWorldTest, PreparedFieldAdoptionUsesCapturedBytesAndMissingMemberClears) {
    ASSERT_NO_FATAL_FAILURE(CheckCapturedFieldAdoptionAndMissingMember());
}

TEST(GameSessionHeadlessWorldTest, EmptySessionFieldsPreserveSaveBytesAndDigests) {
    for (const bool active_regions : {false, true}) {
        ASSERT_NO_FATAL_FAILURE(CheckEmptySessionFieldCompatibility(active_regions));
    }
}

namespace {

using Luminumbra::world::ActiveRegionLedger;
using Luminumbra::world::ActiveRegionRecord;
using Luminumbra::world::RegionKey;
using Luminumbra::world::RegionState;
using Luminumbra::world::WorldClock;

ActiveRegionLedger ScheduledFieldLedger(std::uint64_t tick,
                                        const std::map<RegionKey, RegionState>& owners,
                                        Luminumbra::world::RegionSchedulerConfig config = {
                                            std::numeric_limits<std::uint64_t>::max(), 10000, 2}) {
    const WorldClock clock(tick);
    ActiveRegionLedger ledger(config, clock);
    for (const auto& [key, state] : owners) {
        ledger.pin(key, true, clock);
        // Construct a typed fixture locally, then admit it through the production codecs.
        auto& record =
            const_cast<std::map<RegionKey, ActiveRegionRecord>&>(ledger.records()).at(key);
        record.wake_pending = false;
        record.state = state;
        record.cadence_shift =
            state == RegionState::Active ? std::uint8_t{0} : config.reduced_shift;
        record.cadence_phase = ActiveRegionLedger::phase(key, record.cadence_shift);
        if (state == RegionState::Frozen) {
            record.frozen_at = tick;
            record.frozen_digest = 123;
        }
    }
    return ledger;
}

void LoadScheduledFieldFixture(GameSession& session,
                               const HeadlessRoot& root,
                               JobSystem& jobs,
                               const ActiveRegionLedger& ledger,
                               const FieldPageState& fields,
                               const std::optional<std::string>& energy = std::nullopt) {
    EnableSnapshotSession(session, root, jobs);
    ASSERT_TRUE(session.CreateWorld("Scheduled fields", "1337", "default"));
    const auto input = root.path() / "scheduled-field-input";
    fs::create_directories(input);
    nlohmann::json metadata = {{"container_version", P::WorldSaveService::kContainerVersion}};
    WorldClock(ledger.tick()).write_metadata(metadata);
    {
        P::WorldSaveService::SaveFactsScope facts(input);
        P::SaveTransaction transaction(
            input, ledger.tick(), {}, &P::WorldSaveService::member_digest);
        std::vector<std::string> errors;
        ASSERT_TRUE(transaction.begin(&errors));
        auto saved_ledger = ledger;
        ASSERT_TRUE(P::WorldSaveService::save_metadata_and_active_regions(
            metadata.dump(), saved_ledger, input, &errors));
        ASSERT_TRUE(P::WorldSaveService::save_field_pages(fields, input, &errors));
        if (energy && !energy->empty()) {
            ASSERT_TRUE(transaction.protect({"aether_state.efs"}, &errors));
            ASSERT_TRUE(P::WorldSaveService::write_member_bytes(
                input, "aether_state.efs", *energy, &errors))
                << ::testing::PrintToString(errors);
        }
        ASSERT_TRUE(P::WorldSaveService::finalize_indexed_envelope(input, &errors));
        ASSERT_TRUE(transaction.commit(&errors));
    }
    ASSERT_TRUE(session.LoadWorldStateFrom(input)) << session.GetWorldOpenError();
    ASSERT_EQ(session.GetFieldPages().serialize(), fields.serialize());
    session.SetReplicatedSimulationAnchors({});
}

void CheckScheduledFieldOrderAndCompletedWork() {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, reopened;
    const RegionKey left{0, 0}, right{1, 0};
    auto ledger =
        ScheduledFieldLedger(31, {{left, RegionState::Active}, {right, RegionState::Active}});
    ledger.pin(left, false, WorldClock(31));
    FieldPageState fields;
    ASSERT_NO_FATAL_FAILURE(PopulateSessionFieldFixture(fields, 1337));
    ASSERT_TRUE(fields.activate_region(right, 1337, 0));
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    const auto expected_schedule =
        ledger.schedule(WorldClock(32), std::span<const Luminumbra::Vec3>{});
    // A pinned higher key precedes the lower key. Consumers must retain this order.
    ASSERT_EQ(expected_schedule.due, (std::vector<RegionKey>{right, left}));
    const auto previous = fields;
    auto work = fields.step_scheduled(ledger.records(), expected_schedule.due, 1337, 32, &previous);
    ASSERT_TRUE(fields.set_weather_age(left, 8));
    ASSERT_TRUE(fields.set_weather_age(right, 1));
    ++work[left];
    ++work[right];
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_EQ(session.GetRegionSchedule(), expected_schedule);
    EXPECT_EQ(session.GetFieldPages().serialize(), fields.serialize());
    EXPECT_NE(fields.serialize(), previous.serialize());
    // Left: 441 cells + 2*840 interior edges + 21 shared edges + one weather clock.
    // Right has 462 cells and its own weather clock.
    EXPECT_EQ(work.at(left), 2143u);
    EXPECT_EQ(work.at(right), 2246u);
    EXPECT_EQ(session.GetActiveRegionLedger().completed_work(), work);
    EXPECT_EQ(session.GetRegionSchedule().work, 0u);
    EXPECT_EQ(session.GetFieldPages().weather_clocks(), fields.weather_clocks());
    EXPECT_EQ(session.GetFieldPages().scent_cells(), previous.scent_cells());
    EXPECT_EQ(session.GetFieldPages().soil().pages(), previous.soil().pages());
    EXPECT_EQ(session.GetFieldPages().irrigation().pages(), previous.irrigation().pages());

    const auto save = root.path() / "scheduled-field-continuation";
    ASSERT_TRUE(session.SaveWorldStateTo(save));
    EXPECT_EQ(session.GetFieldPages().serialize(), fields.serialize());
    EnableSnapshotSession(reopened, root, jobs);
    ASSERT_TRUE(reopened.CreateTransientWorld("Field continuation", "1337", "default"));
    ASSERT_TRUE(reopened.LoadWorldStateFrom(save)) << reopened.GetWorldOpenError();
    reopened.SetReplicatedSimulationAnchors({});
    EXPECT_EQ(reopened.GetActiveRegionLedger().completed_work(), work);
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_EQ(reopened.TickSimulation(reopened.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    ASSERT_FALSE(reopened.GetRegionSchedulerFault());
    EXPECT_EQ(session.GetRegionSchedule().work, 4389u);
    EXPECT_EQ(reopened.GetRegionSchedule(), session.GetRegionSchedule());
    EXPECT_EQ(reopened.GetFieldPages().serialize(), session.GetFieldPages().serialize());
    EXPECT_EQ(reopened.GetActiveRegionLedger().canonical_bytes(),
              session.GetActiveRegionLedger().canonical_bytes());
}

void CheckFrozenSessionFieldOwner() {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    const RegionKey left{0, 0}, right{1, 0};
    const auto ledger =
        ScheduledFieldLedger(31, {{left, RegionState::Active}, {right, RegionState::Frozen}});
    FieldPageState fields;
    ASSERT_TRUE(fields.activate_region(left, 1337, 0));
    ASSERT_TRUE(fields.activate_region(right, 1337, 0));
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_EQ(session.GetRegionSchedule().due, (std::vector<RegionKey>{left}));
    EXPECT_EQ(session.GetFieldPages().region_bytes(right), fields.region_bytes(right));
    EXPECT_NE(session.GetFieldPages().region_bytes(left), fields.region_bytes(left));
    EXPECT_EQ(session.GetActiveRegionLedger().completed_work(),
              (std::map<RegionKey, std::uint64_t>{{left, 2122}}));
    EXPECT_EQ(session.GetActiveRegionLedger().records().at(right).frozen_digest, 123u);
}

void CheckSessionFieldCommonBorders() {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    const RegionKey left{2, 0}, right{3, 0};
    const auto ledger =
        ScheduledFieldLedger(31, {{left, RegionState::Reduced}, {right, RegionState::Reduced}});
    ASSERT_EQ(ledger.records().at(left).cadence_phase, 3u);
    ASSERT_EQ(ledger.records().at(right).cadence_phase, 2u);
    FieldPageState fields;
    ASSERT_TRUE(fields.activate_region(left, 1337, 0));
    ASSERT_TRUE(fields.activate_region(right, 1337, 0));
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    const auto before = fields;
    const auto work = fields.step_scheduled(ledger.records(), {}, 1337, 32, &before);
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_TRUE(session.GetRegionSchedule().due.empty());
    EXPECT_EQ(work, (std::map<RegionKey, std::uint64_t>{{left, 21}, {right, 21}}));
    EXPECT_EQ(session.GetActiveRegionLedger().completed_work(), work);
    EXPECT_EQ(session.GetFieldPages().serialize(), fields.serialize());
    EXPECT_NE(fields.region_bytes(left), before.region_bytes(left));
    EXPECT_NE(fields.region_bytes(right), before.region_bytes(right));
    // Tick 33 is neither owner's interior phase nor the common border phase.
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_TRUE(session.GetRegionSchedule().due.empty());
    EXPECT_EQ(session.GetRegionSchedule().work, 42u);
    EXPECT_EQ(session.GetFieldPages().serialize(), fields.serialize());
    EXPECT_TRUE(session.GetActiveRegionLedger().completed_work().empty());
}

void CheckScheduledFieldFaultRollback(bool standard_exception) {
    SCOPED_TRACE(standard_exception);
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    const auto ledger = ScheduledFieldLedger(31, {{kFieldOwner, RegionState::Active}});
    FieldPageState fields;
    ASSERT_NO_FATAL_FAILURE(PopulateSessionFieldFixture(fields, 1337));
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    const auto bytes = fields.serialize();
    EXPECT_EQ(session.TickSimulation(0), 0u);
    EXPECT_EQ(session.GetFieldPages().serialize(), bytes);
    int calls = 0;
    session.GetSimulationEventBus().subscribe([&](const auto&) {
        ++calls;
        if (standard_exception)
            throw std::runtime_error("injected scheduled-field consumer fault");
        throw 7;
    });
    session.GetSimulationEventBus().publish(32, "field-test", "");
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    EXPECT_EQ(calls, 1);
    const auto fault = session.GetRegionSchedulerFault();
    ASSERT_TRUE(fault.has_value());
    if (fault.has_value()) {
        EXPECT_EQ(fault->tick, 32u);
        EXPECT_EQ(fault->message,
                  standard_exception ? "injected scheduled-field consumer fault"
                                     : "Unknown simulation consumer fault");
    }
    EXPECT_EQ(session.GetFieldPages().serialize(), bytes);
    EXPECT_TRUE(session.GetActiveRegionLedger().completed_work().empty());
    EXPECT_TRUE(session.IsSimulationTickBoundary());
    EXPECT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 0u);
    EXPECT_EQ(session.GetFieldPages().serialize(), bytes);
    const auto save = root.path() / "faulted-field-state";
    ASSERT_TRUE(session.SaveWorldStateTo(save));
    ASSERT_NO_FATAL_FAILURE(ExpectIndexedSessionFields(save, fields));
}

void CheckScheduledFieldFreeze(bool corrupt_digest) {
    SCOPED_TRACE(corrupt_digest);
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    const auto ledger = ScheduledFieldLedger(31, {{kFieldOwner, RegionState::Reduced}}, {0, 1, 2});
    FieldPageState fields;
    ASSERT_NO_FATAL_FAILURE(PopulateSessionFieldFixture(fields, 1337));
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    const auto input = root.path() / "scheduled-field-input";
    if (corrupt_digest) {
        const auto path = P::WorldSaveService::region_file_path(input, 0, 0);
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << "not a region file";
    }
    session.RecordActiveRegionWork({kFieldOwner, 1, 0, 0, 0});
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    EXPECT_EQ(session.GetFieldPages().serialize(), fields.serialize());
    EXPECT_TRUE(session.GetActiveRegionLedger().completed_work().empty());
    EXPECT_EQ(session.GetRegionSchedulerFault().has_value(), corrupt_digest);
    if (corrupt_digest) {
        const auto fault = session.GetRegionSchedulerFault();
        ASSERT_TRUE(fault.has_value());
        if (fault.has_value()) {
            EXPECT_FALSE(fault->message.empty());
        }
        EXPECT_EQ(session.GetActiveRegionLedger().tick(), 31u);
        EXPECT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 0u);
    } else {
        const auto& record = session.GetActiveRegionLedger().records().at(kFieldOwner);
        EXPECT_EQ(record.state, RegionState::Frozen);
        auto inputs = session.GetWorldSystem()->region_digest_inputs(0, 0);
        inputs.require_index = true;
        inputs.field_page_bytes = fields.region_bytes(kFieldOwner);
        EXPECT_EQ(record.frozen_digest,
                  P::WorldSaveService::region_simulation_digest(input, kFieldOwner, inputs));
    }
}

void CheckSessionFieldOwnershipRetention() {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    const RegionKey unowned{1, 0};
    const auto tick = ActiveRegionLedger::kIdleEvictionTicks;
    auto ledger = ScheduledFieldLedger(
        tick, {{kFieldOwner, RegionState::Frozen}, {unowned, RegionState::Active}});
    ledger.pin(kFieldOwner, false, WorldClock(tick));
    ledger.pin(unowned, false, WorldClock(tick));
    FieldPageState fields;
    ASSERT_TRUE(fields.set_weather_age(kFieldOwner, 7));
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_EQ(session.GetActiveRegionLedger().records().size(), 1u);
    EXPECT_TRUE(session.GetActiveRegionLedger().records().contains(kFieldOwner));
    EXPECT_FALSE(session.GetActiveRegionLedger().records().contains(unowned));
    EXPECT_EQ(session.GetFieldPages().serialize(), fields.serialize());
    EXPECT_TRUE(session.GetActiveRegionLedger().completed_work().empty());
}

} // namespace

TEST(GameSessionHeadlessWorldTest, ScheduledFieldsPreserveOrderAndCompletedWorkAcrossReopen) {
    ASSERT_NO_FATAL_FAILURE(CheckScheduledFieldOrderAndCompletedWork());
}

TEST(GameSessionHeadlessWorldTest, ScheduledFieldsLeaveFrozenOwnersAndTheirBordersUnchanged) {
    ASSERT_NO_FATAL_FAILURE(CheckFrozenSessionFieldOwner());
}

TEST(GameSessionHeadlessWorldTest, ScheduledFieldsExchangeOnCommonPhaseWithoutDueInteriors) {
    ASSERT_NO_FATAL_FAILURE(CheckSessionFieldCommonBorders());
}

TEST(GameSessionHeadlessWorldTest, ScheduledFieldsRemainUnchangedWhenTheTickFaults) {
    for (const bool standard_exception : {false, true}) {
        ASSERT_NO_FATAL_FAILURE(CheckScheduledFieldFaultRollback(standard_exception));
    }
}

TEST(GameSessionHeadlessWorldTest, ScheduledFieldFreezeIncludesPagesAndRefusalPreservesState) {
    for (const bool corrupt_digest : {false, true}) {
        ASSERT_NO_FATAL_FAILURE(CheckScheduledFieldFreeze(corrupt_digest));
    }
}

TEST(GameSessionHeadlessWorldTest, FieldEvolutionOwnersSurviveIdleLedgerRetention) {
    ASSERT_NO_FATAL_FAILURE(CheckSessionFieldOwnershipRetention());
}

namespace {

using Luminumbra::world::RegionalStorm;

RegionalStorm SessionStorm(RegionKey owner,
                           float x,
                           float z,
                           float vx = 0,
                           float vz = 0,
                           std::uint32_t age = 10,
                           std::uint32_t salt = 99) {
    return {owner,
            std::bit_cast<std::uint32_t>(x),
            std::bit_cast<std::uint32_t>(z),
            std::bit_cast<std::uint32_t>(vx),
            std::bit_cast<std::uint32_t>(vz),
            std::bit_cast<std::uint32_t>(1.0f),
            0,
            240,
            salt,
            age};
}

void ExpectWeatherContinuation(GameSession& original, GameSession& reopened) {
    EXPECT_FALSE(original.GetRegionSchedulerFault());
    EXPECT_FALSE(reopened.GetRegionSchedulerFault());
    EXPECT_EQ(reopened.GetFieldPages().serialize(), original.GetFieldPages().serialize());
    EXPECT_EQ(reopened.GetRegionSchedule(), original.GetRegionSchedule());
    EXPECT_EQ(reopened.GetActiveRegionLedger().canonical_bytes(),
              original.GetActiveRegionLedger().canonical_bytes());
    EXPECT_EQ(SessionWorldSubHashes(reopened), SessionWorldSubHashes(original));
}

void ReopenSessionWeather(GameSession& original,
                          GameSession& reopened,
                          const HeadlessRoot& root,
                          JobSystem& jobs) {
    const auto save = root.path() / "weather-continuation";
    const auto fields = original.GetFieldPages().serialize();
    ASSERT_TRUE(original.SaveWorldStateTo(save));
    EXPECT_EQ(original.GetFieldPages().serialize(), fields);
    EnableSnapshotSession(reopened, root, jobs);
    ASSERT_TRUE(reopened.CreateTransientWorld("Weather continuation", "1337", "default"));
    ASSERT_TRUE(reopened.LoadWorldStateFrom(save)) << reopened.GetWorldOpenError();
    reopened.SetReplicatedSimulationAnchors({});
    EXPECT_EQ(reopened.GetFieldPages().serialize(), fields);
    EXPECT_EQ(SessionWorldSubHashes(reopened), SessionWorldSubHashes(original));
    EXPECT_TRUE(reopened.GetWeatherSystem()->StrikesThisTick().empty());
}

void CheckWeatherReopenAndStrike(bool frozen_target) {
    SCOPED_TRACE(frozen_target);
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, reopened, after_strike;
    const RegionKey source{0, -1}, target{1, -1};
    const auto ledger = ScheduledFieldLedger(
        1003,
        {{source, RegionState::Active},
         {target, frozen_target ? RegionState::Frozen : RegionState::Reduced}});
    FieldPageState fields;
    ASSERT_TRUE(fields.set_weather_age(source, 44));
    ASSERT_TRUE(fields.replace_weather({SessionStorm(source, 500, -256, 0, 0, 53, 4)}, {}));
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    ASSERT_NO_FATAL_FAILURE(ReopenSessionWeather(session, reopened, root, jobs));
    TickOnce(session);
    TickOnce(reopened);
    ASSERT_NO_FATAL_FAILURE(ExpectWeatherContinuation(session, reopened));
    EXPECT_EQ(session.GetFieldPages().weather_clocks().at(source), 45u);
    const auto& storms = session.GetFieldPages().storms();
    ASSERT_EQ(storms.size(), 2u);
    EXPECT_EQ(storms.back().spawn_tick, 1004u);
    EXPECT_EQ(storms.back().active_age, 0u);
    EXPECT_EQ(storms.back().seed_salt, 1812730280u);
    const auto& strikes = session.GetFieldPages().strikes();
    EXPECT_EQ(strikes.size(), frozen_target ? 0u : 1u);
    EXPECT_EQ(session.GetWeatherSystem()->StrikesThisTick().size(), strikes.size());
    if (!frozen_target) {
        ASSERT_EQ(strikes.size(), 1u);
        EXPECT_EQ(strikes.front().owner, target);
        EXPECT_EQ(strikes.front().strike_tick, 1004u);
        EXPECT_EQ(strikes.front().storm_salt, 4u);
        EXPECT_FLOAT_EQ(std::bit_cast<float>(strikes.front().magnitude), 0.91478163f);
    }
    ASSERT_NO_FATAL_FAILURE(ReopenSessionWeather(session, after_strike, root, jobs));
    TickOnce(session);
    TickOnce(reopened);
    TickOnce(after_strike);
    ASSERT_NO_FATAL_FAILURE(ExpectWeatherContinuation(session, reopened));
    ASSERT_NO_FATAL_FAILURE(ExpectWeatherContinuation(session, after_strike));
    EXPECT_TRUE(after_strike.GetWeatherSystem()->StrikesThisTick().empty());
}

void CheckWeatherHandover() {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, reopened;
    const RegionKey source{0, -1}, target{1, -1};
    const auto ledger = ScheduledFieldLedger(
        1002, {{source, RegionState::Reduced}, {target, RegionState::Reduced}});
    FieldPageState fields;
    // A due weather owner admits the candidate even while the source interior is idle.
    ASSERT_TRUE(fields.set_weather_age(target, 0));
    const auto storm = SessionStorm(source, 511.75f, -256, 30, 0);
    ASSERT_TRUE(fields.replace_weather({storm}, {}));
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    TickOnce(session);
    ASSERT_EQ(session.GetFieldPages().storms().size(), 1u);
    EXPECT_EQ(session.GetFieldPages().storms().front(), storm);
    TickOnce(session);
    EXPECT_EQ(session.GetRegionSchedule().due, (std::vector<RegionKey>{target}));
    ASSERT_EQ(session.GetFieldPages().storms().size(), 1u);
    const auto& moved = session.GetFieldPages().storms().front();
    EXPECT_EQ(moved.owner, target);
    EXPECT_FLOAT_EQ(std::bit_cast<float>(moved.center_x), 512.75f);
    EXPECT_EQ(moved.active_age, storm.active_age);
    EXPECT_EQ(moved.seed_salt, storm.seed_salt);
    EXPECT_EQ(session.GetActiveRegionLedger().completed_work(),
              (std::map<RegionKey, std::uint64_t>{{source, 1}, {target, 2}}));
    ASSERT_NO_FATAL_FAILURE(ReopenSessionWeather(session, reopened, root, jobs));
    TickOnce(session);
    TickOnce(reopened);
    ASSERT_NO_FATAL_FAILURE(ExpectWeatherContinuation(session, reopened));
}

void CheckWeatherBoundaryTargetAdmission(bool frozen_target) {
    SCOPED_TRACE(frozen_target);
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    const RegionKey source{0, -1}, target{1, -1}, due_owner{4, -1};
    std::map<RegionKey, RegionState> owners{{source, RegionState::Reduced},
                                            {due_owner, RegionState::Active}};
    if (frozen_target) {
        owners.emplace(target, RegionState::Frozen);
    }
    const auto ledger = ScheduledFieldLedger(1003, owners);
    FieldPageState fields;
    const auto storm = SessionStorm(source, 511.75f, -256, 30, 0);
    ASSERT_TRUE(fields.replace_weather({storm}, {}));
    ASSERT_TRUE(fields.set_weather_age(due_owner, 0));
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    session.PinActiveRegion(target, true);
    TickOnce(session);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    ASSERT_EQ(session.GetFieldPages().storms().size(), 1u);
    auto expected = storm;
    expected.owner = target;
    expected.center_x = std::bit_cast<std::uint32_t>(512.75f);
    EXPECT_EQ(session.GetFieldPages().storms().front(), expected);
    EXPECT_FALSE(session.GetFieldPages().weather_clocks().contains(target));
    EXPECT_EQ(session.GetActiveRegionLedger().completed_work(),
              (std::map<RegionKey, std::uint64_t>{{source, 1}, {target, 1}, {due_owner, 1}}));
}

void CheckWeatherWithoutDueAuthority(bool unrelated_weather, int page_kind) {
    SCOPED_TRACE(unrelated_weather);
    SCOPED_TRACE(page_kind);
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    const RegionKey source{0, -1}, target{1, -1}, unrelated{4, -1};
    std::map<RegionKey, RegionState> owners{{source, RegionState::Reduced},
                                            {target, RegionState::Reduced},
                                            {unrelated, RegionState::Active}};
    FieldPageState fields;
    if (page_kind != 0) {
        // The nearby frozen owner shares a physical page with the empty destination.
        const RegionKey frozen{page_kind == 1 ? 8 : 2, -1};
        owners.emplace(frozen, RegionState::Frozen);
        ASSERT_TRUE(fields.activate_region(frozen, 1337, 1003));
    }
    const auto ledger = ScheduledFieldLedger(1003, owners);
    const auto storm = SessionStorm(source, 511.75f, -256, 30, 0);
    ASSERT_TRUE(fields.replace_weather({storm}, {}));
    if (unrelated_weather) {
        ASSERT_TRUE(fields.set_weather_age(unrelated, 0));
    }
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    int observations = 0;
    session.GetSimulationEventBus().subscribe([&](const auto& event) {
        if (event.topic != "idle-weather-test")
            return;
        ++observations;
        EXPECT_EQ(session.GetRegionalWeatherCandidateRecordCount(), unrelated_weather ? 2u : 1u);
        EXPECT_EQ(session.GetRegionalWeatherProjectionCount(), 1u);
    });
    session.GetSimulationEventBus().publish(1004, "idle-weather-test", "");
    TickOnce(session);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_EQ(observations, 1);
    const auto& due = session.GetRegionSchedule().due;
    EXPECT_NE(std::find(due.begin(), due.end(), target), due.end());
    ASSERT_EQ(session.GetFieldPages().storms().size(), 1u);
    auto expected = storm;
    expected.owner = target;
    expected.center_x = std::bit_cast<std::uint32_t>(512.75f);
    EXPECT_EQ(session.GetFieldPages().storms().front(), expected);
    EXPECT_EQ(session.GetFieldPages().pages(), fields.pages());
    EXPECT_FALSE(session.GetFieldPages().weather_clocks().contains(target));
    auto expected_work = std::map<RegionKey, std::uint64_t>{{source, 1}, {target, 1}};
    if (unrelated_weather) {
        expected_work.emplace(unrelated, 1);
    }
    EXPECT_EQ(session.GetActiveRegionLedger().completed_work(), expected_work);
}

void CheckWeatherWaitsForDestination(bool unrelated_weather, bool active_source) {
    SCOPED_TRACE(unrelated_weather);
    SCOPED_TRACE(active_source);
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, reopened;
    const RegionKey source{0, -1}, unrelated{4, -1};
    const std::array neighbours{
        RegionKey{1, -1}, RegionKey{-1, -1}, RegionKey{0, 0}, RegionKey{0, -2}};
    const auto selected = std::find_if(neighbours.begin(), neighbours.end(), [](RegionKey owner) {
        return ActiveRegionLedger::phase(owner, 2) != 0;
    });
    ASSERT_NE(selected, neighbours.end());
    const auto target = *selected;
    const auto dx = target.x - source.x, dz = target.z - source.z;
    const float x = dx > 0 ? 511.75f : dx < 0 ? 0.25f : 256.0f;
    const float z = dz > 0 ? -0.25f : dz < 0 ? -511.75f : -256.0f;
    const auto storm =
        SessionStorm(source, x, z, static_cast<float>(dx) * 30, static_cast<float>(dz) * 30, 60);
    const auto ledger =
        ScheduledFieldLedger(1003,
                             {{source, active_source ? RegionState::Active : RegionState::Reduced},
                              {target, RegionState::Reduced},
                              {unrelated, RegionState::Active}});
    FieldPageState fields;
    ASSERT_TRUE(fields.replace_weather({storm}, {}));
    if (unrelated_weather) {
        ASSERT_TRUE(fields.set_weather_age(unrelated, 0));
    }
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    if (active_source) {
        session.GetWindFieldSystem()->BindRegionalBackend(
            [dx, dz](const Luminumbra::Vec3&, Luminumbra::Systems::WindLayer) {
                return Luminumbra::Vec2(static_cast<float>(dx) * 30, static_cast<float>(dz) * 30);
            },
            [] { return std::string("border wind"); });
    }
    int observations = 0;
    session.GetSimulationEventBus().subscribe([&](const auto& event) {
        if (event.topic == "closed-weather-target") {
            ++observations;
            EXPECT_EQ(session.GetRegionalWeatherCandidateRecordCount(),
                      static_cast<std::size_t>(unrelated_weather) +
                          static_cast<std::size_t>(active_source));
            EXPECT_EQ(session.GetRegionalWeatherProjectionCount(), active_source ? 1u : 0u);
        }
    });
    session.GetSimulationEventBus().publish(1004, "closed-weather-target", "");
    TickOnce(session);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_EQ(observations, 1);
    const auto& due = session.GetRegionSchedule().due;
    EXPECT_EQ(std::find(due.begin(), due.end(), source) != due.end(), active_source);
    EXPECT_EQ(std::find(due.begin(), due.end(), target), due.end());
    ASSERT_EQ(session.GetFieldPages().storms().size(), 1u);
    auto expected = storm;
    if (active_source) {
        ++expected.active_age;
        const float border_x = dx > 0 ? std::nextafter(512.0f, 0.0f) : dx < 0 ? 0.0f : x;
        const float border_z = dz > 0 ? std::nextafter(0.0f, -512.0f) : dz < 0 ? -512.0f : z;
        expected.center_x = std::bit_cast<std::uint32_t>(border_x);
        expected.center_z = std::bit_cast<std::uint32_t>(border_z);
    }
    EXPECT_EQ(session.GetFieldPages().storms().front(), expected);
    EXPECT_EQ(session.GetActiveRegionLedger().completed_work().contains(source), active_source);
    EXPECT_FALSE(session.GetActiveRegionLedger().completed_work().contains(target));
    if (active_source) {
        return;
    }
    ASSERT_NO_FATAL_FAILURE(ReopenSessionWeather(session, reopened, root, jobs));
    TickOnce(session);
    TickOnce(reopened);
    ASSERT_NO_FATAL_FAILURE(ExpectWeatherContinuation(session, reopened));
}

void CheckStationaryWeatherProjection() {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    const auto ledger = ScheduledFieldLedger(1003, {{kFieldOwner, RegionState::Active}});
    FieldPageState fields;
    // Supply still wind so only active age changes at the mature intensity plateau.
    const auto storm = SessionStorm(kFieldOwner, 108, 108, 0, 0, 60);
    ASSERT_TRUE(fields.replace_weather({storm}, {}));
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    session.GetWindFieldSystem()->BindRegionalBackend(
        [](const Luminumbra::Vec3&, Luminumbra::Systems::WindLayer) { return Luminumbra::Vec2(0); },
        [] { return std::string("still wind"); });
    TickOnce(session);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    ASSERT_EQ(session.GetFieldPages().storms().size(), 1u);
    auto expected = storm;
    ++expected.active_age;
    EXPECT_EQ(session.GetFieldPages().storms().front(), expected);
    EXPECT_EQ(session.GetWeatherSystem()->RegionalStormNodeChanges(), 0u);
    EXPECT_EQ(session.GetWeatherSystem()->RegionalProjectionWork(), 0u);
}

void CheckWeatherSpawnCapacity(std::size_t count, bool expires) {
    SCOPED_TRACE(count);
    SCOPED_TRACE(expires);
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, control;
    const RegionKey owner{0, -1};
    const auto ledger = ScheduledFieldLedger(1003, {{owner, RegionState::Active}});
    FieldPageState fields, empty;
    ASSERT_TRUE(fields.set_weather_age(owner, 44));
    ASSERT_TRUE(empty.set_weather_age(owner, 44));
    std::vector<RegionalStorm> storms;
    for (std::size_t i = 0; i < count; ++i)
        storms.push_back(SessionStorm(owner,
                                      256,
                                      -256,
                                      0,
                                      0,
                                      expires && i == 0 ? 239 : 10,
                                      static_cast<std::uint32_t>(i + 1)));
    ASSERT_TRUE(fields.replace_weather(storms, {}));
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(control, root, jobs, ledger, empty));
    TickOnce(control);
    ASSERT_FALSE(control.GetRegionSchedulerFault());
    ASSERT_EQ(control.GetFieldPages().storms().size(), 1u); // Observe a successful spawn draw.
    TickOnce(session);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    const auto survivors = count - static_cast<std::size_t>(expires);
    EXPECT_EQ(session.GetFieldPages().storms().size(),
              std::min(survivors + 1, FieldPageState::kMaxOwnerStorms));
    EXPECT_EQ(session.GetFieldPages().weather_clocks().at(owner), 45u);
    EXPECT_EQ(std::count_if(session.GetFieldPages().storms().begin(),
                            session.GetFieldPages().storms().end(),
                            [](const auto& storm) { return storm.spawn_tick == 1004; }),
              survivors < FieldPageState::kMaxOwnerStorms ? 1 : 0);
}

void CheckWeatherTransferCapacity(std::size_t count,
                                  bool expires = false,
                                  std::size_t arrivals = 1) {
    SCOPED_TRACE(count);
    SCOPED_TRACE(expires);
    SCOPED_TRACE(arrivals);
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    const RegionKey source{0, -1}, target{1, -1};
    const auto ledger = ScheduledFieldLedger(
        1003, {{source, RegionState::Reduced}, {target, RegionState::Reduced}});
    std::vector<RegionalStorm> incoming;
    for (std::size_t i = 0; i < arrivals; ++i)
        incoming.push_back(
            SessionStorm(source, 511.75f, -256, 30, 0, 10, static_cast<std::uint32_t>(i * 99)));
    auto storms = incoming;
    for (std::size_t i = 0; i < count; ++i)
        storms.push_back(SessionStorm(target,
                                      768,
                                      -256,
                                      0,
                                      0,
                                      expires && i == count - 1 ? 239 : 10,
                                      static_cast<std::uint32_t>(i + 1)));
    FieldPageState fields;
    ASSERT_TRUE(fields.replace_weather(storms, {}));
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    TickOnce(session);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    const auto& after = session.GetFieldPages().storms();
    const auto survivors = count - static_cast<std::size_t>(expires);
    ASSERT_EQ(after.size(), survivors + arrivals);
    const auto admitted = std::min(arrivals, FieldPageState::kMaxOwnerStorms - survivors);
    for (std::size_t i = 0; i < incoming.size(); ++i) {
        const auto found = std::find_if(after.begin(), after.end(), [&](const auto& storm) {
            return storm.seed_salt == incoming[i].seed_salt;
        });
        ASSERT_NE(found, after.end());
        EXPECT_EQ(found->owner, i < admitted ? target : source);
        EXPECT_EQ(found->active_age, incoming[i].active_age);
        if (i >= admitted) {
            EXPECT_EQ(*found, incoming[i]);
        }
    }
    EXPECT_EQ(std::count_if(after.begin(),
                            after.end(),
                            [target](const auto& storm) { return storm.owner == target; }),
              survivors + admitted);
}

void CheckWeatherCorner(int blocked) {
    SCOPED_TRACE(blocked);
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    const RegionKey source{0, 0}, target{1, 1}, east{1, 0}, north{0, 1};
    std::map<RegionKey, RegionState> owners{{source, RegionState::Reduced},
                                            {target, RegionState::Active},
                                            {east, RegionState::Reduced},
                                            {north, RegionState::Reduced}};
    if (blocked == 1)
        owners.erase(east);
    if (blocked == 2)
        owners[north] = RegionState::Frozen;
    if (blocked == 3)
        owners[target] = RegionState::Frozen;
    const auto ledger = ScheduledFieldLedger(1003, owners);
    FieldPageState fields;
    ASSERT_TRUE(fields.set_weather_age(target, 0));
    const auto storm = SessionStorm(source, 511.75f, 511.75f, 30, 30);
    ASSERT_TRUE(fields.replace_weather({storm}, {}));
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    TickOnce(session);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    ASSERT_EQ(session.GetFieldPages().storms().size(), 1u);
    const auto& after = session.GetFieldPages().storms().front();
    EXPECT_EQ(after.owner, blocked == 0 ? target : source);
    EXPECT_FLOAT_EQ(std::bit_cast<float>(after.center_x), blocked == 0 ? 512.75f : 511.75f);
    EXPECT_FLOAT_EQ(std::bit_cast<float>(after.center_z), blocked == 0 ? 512.75f : 511.75f);
    EXPECT_EQ(after.active_age, storm.active_age);
    const auto& work = session.GetActiveRegionLedger().completed_work();
    if (blocked == 3) {
        EXPECT_TRUE(work.empty());
    } else {
        EXPECT_EQ(work.at(target), blocked == 0 ? 2u : 1u);
        EXPECT_EQ(work.contains(source), blocked == 0);
    }
}

void CheckWeatherFrozenAndExpiry(bool frozen) {
    SCOPED_TRACE(frozen);
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    const RegionKey owner{0, 0};
    const auto ledger =
        ScheduledFieldLedger(1003, {{owner, frozen ? RegionState::Frozen : RegionState::Reduced}});
    FieldPageState fields;
    ASSERT_TRUE(fields.set_weather_age(owner, 7));
    const auto storm = SessionStorm(owner, 256, 256, 0, 0, 239);
    const Luminumbra::world::RegionalStrike strike{owner,
                                                   1003,
                                                   std::bit_cast<std::uint32_t>(256.0f),
                                                   std::bit_cast<std::uint32_t>(256.0f),
                                                   std::bit_cast<std::uint32_t>(0.5f),
                                                   99,
                                                   0};
    ASSERT_TRUE(fields.replace_weather({storm}, {strike}));
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    EXPECT_TRUE(session.GetWeatherSystem()->StrikesThisTick().empty());
    TickOnce(session); // common phase only
    EXPECT_EQ(session.GetFieldPages().serialize(), fields.serialize());
    TickOnce(session); // owner's reduced interior phase
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    if (frozen) {
        EXPECT_EQ(session.GetFieldPages().serialize(), fields.serialize());
        EXPECT_TRUE(session.GetActiveRegionLedger().completed_work().empty());
    } else {
        EXPECT_TRUE(session.GetFieldPages().storms().empty());
        EXPECT_TRUE(session.GetFieldPages().strikes().empty());
        EXPECT_EQ(session.GetFieldPages().weather_clocks().at(owner), 8u);
        EXPECT_EQ(session.GetActiveRegionLedger().completed_work().at(owner), 3u);
    }
    EXPECT_TRUE(session.GetWeatherSystem()->StrikesThisTick().empty());
}

void FillWeatherCells(FieldPageState& fields, const std::array<std::uint16_t, 5>& words) {
    using Luminumbra::world::FieldPage;
    using Luminumbra::world::FieldPageKey;
    auto& pages = const_cast<std::map<FieldPageKey, FieldPage>&>(fields.pages());
    for (auto& [key, page] : pages) {
        (void)key;
        for (std::size_t cell = 0; cell < FieldPage::kCells; ++cell) {
            if (((page.mask[cell / 64] >> (cell % 64)) & 1u) == 0)
                continue;
            for (std::size_t channel = 0; channel < words.size(); ++channel)
                page.cells[channel][cell] = words[channel];
        }
    }
}

void CheckWeatherBlockedDueMotion() {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    const RegionKey source{0, 0}, target{1, 1}, east{1, 0}, north{0, 1};
    const auto ledger = ScheduledFieldLedger(1000,
                                             {{source, RegionState::Active},
                                              {target, RegionState::Reduced},
                                              {east, RegionState::Reduced},
                                              {north, RegionState::Reduced}});
    FieldPageState fields;
    ASSERT_TRUE(fields.activate_region(target, 1337, 0));
    FillWeatherCells(fields, {31744, 31744, 32768, 32768, 16384});
    ASSERT_TRUE(fields.replace_weather({SessionStorm(source, 511.99f, 511.99f)}, {}));
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    TickOnce(session); // both interiors due, common border closed
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    ASSERT_EQ(session.GetFieldPages().storms().size(), 1u);
    const auto& storm = session.GetFieldPages().storms().front();
    EXPECT_EQ(storm.owner, source);
    EXPECT_EQ(storm.active_age, 11u);
    EXPECT_FLOAT_EQ(std::bit_cast<float>(storm.center_x), std::nextafter(512.0f, 0.0f));
    EXPECT_FLOAT_EQ(std::bit_cast<float>(storm.center_z), std::nextafter(512.0f, 0.0f));
    std::uint16_t wind_x = 0, wind_z = 0;
    ASSERT_TRUE(session.GetFieldPages().sample(0, 21, 21, wind_x));
    ASSERT_TRUE(session.GetFieldPages().sample(1, 21, 21, wind_z));
    EXPECT_NE(wind_x, 31744u);
    EXPECT_NE(wind_z, 31744u);
    EXPECT_FLOAT_EQ(std::bit_cast<float>(storm.velocity_x),
                    static_cast<float>(std::bit_cast<std::int16_t>(wind_x)) / 1024.0f);
    EXPECT_FLOAT_EQ(std::bit_cast<float>(storm.velocity_z),
                    static_cast<float>(std::bit_cast<std::int16_t>(wind_z)) / 1024.0f);
}

void CheckWeatherCellOwnership() {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, reopened;
    const RegionKey owner{1, 1};
    const auto ledger = ScheduledFieldLedger(1003, {{owner, RegionState::Frozen}});
    FieldPageState fields;
    ASSERT_TRUE(fields.activate_region(owner, 1337, 0));
    // This position's 24m cell is owned by its centre (516, 516), across both seams.
    const Luminumbra::Vec3 position{511.75f, 0, 511.75f};
    FillWeatherCells(fields, {1024, 64512, 32768, 32768, 16384});
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    EXPECT_EQ(FieldPageState::region_of_cell(21, 21), owner);
    EXPECT_EQ(session.GetWindFieldSystem()->SampleWind(position), Luminumbra::Vec2(1, -1));
    EXPECT_EQ(
        session.GetWindFieldSystem()->SampleWind(position, Luminumbra::Systems::WindLayer::Mid),
        Luminumbra::Vec2(2, -2));
    const auto sample = session.GetWeatherSystem()->SampleAt(position);
    EXPECT_EQ(sample.category, Luminumbra::Systems::WeatherCategory::Rain);
    EXPECT_FLOAT_EQ(sample.precip_intensity, 32768.0f / 65535.0f);
    EXPECT_FLOAT_EQ(session.GetAetherFieldSystem()->SampleAether(position), 16384.0f / 65535.0f);
    const auto hashes = SessionWorldSubHashes(session);
    TickOnce(session);
    EXPECT_EQ(session.GetFieldPages().serialize(), fields.serialize());
    const auto after = SessionWorldSubHashes(session);
    EXPECT_EQ(after.at("wind"), hashes.at("wind"));
    EXPECT_EQ(after.at("weather"), hashes.at("weather"));
    EXPECT_EQ(after.at("aether"), hashes.at("aether"));
    ASSERT_NO_FATAL_FAILURE(ReopenSessionWeather(session, reopened, root, jobs));
    EXPECT_EQ(reopened.GetWindFieldSystem()->SampleWind(position), Luminumbra::Vec2(1, -1));
    EXPECT_FLOAT_EQ(reopened.GetAetherFieldSystem()->SampleAether(position), 16384.0f / 65535.0f);
}

void CheckWeatherFailure(bool late_failure) {
    SCOPED_TRACE(late_failure);
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    const auto ledger = ScheduledFieldLedger(31, {{kFieldOwner, RegionState::Active}});
    FieldPageState fields;
    ASSERT_TRUE(
        fields.set_weather_age(kFieldOwner, late_failure ? 44 : WorldClock::kTickLimit - 1));
    if (late_failure) {
        ASSERT_TRUE(fields.replace_weather({SessionStorm(kFieldOwner, 256, 256, 0, 0, 239)}, {}));
    }
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    const auto hashes = SessionWorldSubHashes(session);
    if (late_failure) {
        session.GetSimulationEventBus().subscribe(
            [](const auto&) { throw std::runtime_error("injected weather consumer fault"); });
        session.GetSimulationEventBus().publish(32, "weather-test", "");
    }
    TickOnce(session);
    const auto fault = session.GetRegionSchedulerFault();
    ASSERT_TRUE(fault.has_value());
    if (fault.has_value()) {
        EXPECT_EQ(fault->message,
                  late_failure ? "injected weather consumer fault"
                               : "Regional weather clock admission failed");
    }
    EXPECT_EQ(session.GetFieldPages().serialize(), fields.serialize());
    EXPECT_TRUE(session.GetActiveRegionLedger().completed_work().empty());
    const auto after = SessionWorldSubHashes(session);
    EXPECT_EQ(after.at("wind"), hashes.at("wind"));
    EXPECT_EQ(after.at("weather"), hashes.at("weather"));
    EXPECT_EQ(after.at("aether"), hashes.at("aether"));
    EXPECT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 0u);
}

using FieldWords = std::array<std::uint16_t, FieldPageState::kChannels>;

void AddWeatherOverlayCell(FieldPageState& fields, std::int32_t x, const FieldWords& words) {
    using Luminumbra::world::FieldPage;
    using Luminumbra::world::FieldPageKey;
    auto& pages = const_cast<std::map<FieldPageKey, FieldPage>&>(fields.pages());
    auto& page = pages[{x / FieldPageState::kPageCells, 0}];
    const auto index =
        static_cast<std::size_t>(4 * FieldPageState::kPageCells + x % FieldPageState::kPageCells);
    page.mask[index / 64] |= std::uint64_t{1} << (index % 64);
    for (std::size_t channel = 0; channel < words.size(); ++channel)
        page.cells[channel][index] = words[channel];
}

void PopulateWeatherOverlayFields(FieldPageState& fields, std::size_t unrelated_pages) {
    const FieldWords words{1024, 64512, 32768, 32768, 16384};
    // Both owners share a physical page; only the first owner's cell is due.
    AddWeatherOverlayCell(fields, 20, words);
    AddWeatherOverlayCell(fields, 21, words);
    for (std::size_t i = 0; i < unrelated_pages; ++i)
        AddWeatherOverlayCell(fields, 64 * static_cast<std::int32_t>(i + 1), words);
    ASSERT_TRUE(fields.set_weather_age(kFieldOwner, 7));
    ASSERT_TRUE(fields.set_weather_age({1, 0}, 19));
    const Luminumbra::world::RegionalStrike expired{kFieldOwner,
                                                    31,
                                                    std::bit_cast<std::uint32_t>(492.0f),
                                                    std::bit_cast<std::uint32_t>(108.0f),
                                                    std::bit_cast<std::uint32_t>(0.5f),
                                                    99,
                                                    0};
    auto frozen = expired;
    frozen.owner = {1, 0};
    frozen.world_x = std::bit_cast<std::uint32_t>(516.0f);
    ASSERT_TRUE(fields.replace_weather({SessionStorm(kFieldOwner, 256, 400)}, {expired, frozen}));
    ASSERT_TRUE(fields.set_scent({0, 0}, {std::bit_cast<std::uint64_t>(1.0), 0, 0, 0}));
    ASSERT_TRUE(fields.set_soil({0, 0}, 100));
    ASSERT_TRUE(fields.set_irrigation({0, 0}, 200));
}

FieldWords WeatherOverlayWords(const FieldPageState& fields, std::int32_t x) {
    FieldWords words{};
    for (int channel = 0; channel < FieldPageState::kChannels; ++channel)
        EXPECT_TRUE(fields.sample(channel, x, 4, words[static_cast<std::size_t>(channel)]));
    return words;
}

void ExpectWeatherOverlaySample(GameSession& session,
                                const FieldPageState& expected,
                                std::int32_t x) {
    SCOPED_TRACE(x);
    const auto words = WeatherOverlayWords(expected, x);
    const Luminumbra::Vec3 position{static_cast<float>(x) * 24.0f + 12.0f, 0, 108};
    const Luminumbra::Vec2 wind{static_cast<float>(std::bit_cast<std::int16_t>(words[0])) / 1024.0f,
                                static_cast<float>(std::bit_cast<std::int16_t>(words[1])) /
                                    1024.0f};
    EXPECT_EQ(session.GetWindFieldSystem()->SampleWind(position), wind);
    EXPECT_EQ(
        session.GetWindFieldSystem()->SampleWind(position, Luminumbra::Systems::WindLayer::Mid),
        wind * 2.0f);
    const auto sample = session.GetWeatherSystem()->SampleAt(position);
    using Category = Luminumbra::Systems::WeatherCategory;
    const auto category = words[3] < 13107   ? Category::Clear
                          : words[3] < 26214 ? Category::Overcast
                          : words[3] < 39321 ? Category::Rain
                          : words[3] < 52428 ? Category::Snow
                                             : Category::Fog;
    EXPECT_EQ(sample.category, category);
    EXPECT_EQ(sample.wind, wind);
    EXPECT_FLOAT_EQ(sample.precip_intensity,
                    category == Category::Rain || category == Category::Snow
                        ? static_cast<float>(words[2]) / 65535.0f
                        : 0.0f);
    EXPECT_FLOAT_EQ(session.GetAetherFieldSystem()->SampleAether(position),
                    static_cast<float>(words[4]) / 65535.0f);
}

std::array<std::string, 3> WeatherOverlayHashes(GameSession& session) {
    return {session.GetWindFieldSystem()->ComputeWindSubHash(),
            session.GetWeatherSystem()->ComputeWeatherSubHash(),
            session.GetAetherFieldSystem()->ComputeAetherSubHash()};
}

void ExpectWeatherOverlayPublication(GameSession& session, const FieldPageState& before) {
    const auto& published = session.GetFieldPages();
    ASSERT_EQ(published.storms().size(), before.storms().size());
    const auto& storm = published.storms().front();
    EXPECT_EQ(storm.active_age, before.storms().front().active_age + 1);
    EXPECT_NE(storm, before.storms().front());
    const auto& projected = session.GetWeatherSystem()->StormCells();
    ASSERT_EQ(projected.size(), 1u);
    EXPECT_EQ(storm.center_x, std::bit_cast<std::uint32_t>(projected.front().center_world.x));
    EXPECT_EQ(storm.center_z, std::bit_cast<std::uint32_t>(projected.front().center_world.y));
    EXPECT_EQ(storm.velocity_x, std::bit_cast<std::uint32_t>(projected.front().velocity.x));
    EXPECT_EQ(storm.velocity_z, std::bit_cast<std::uint32_t>(projected.front().velocity.y));
    EXPECT_EQ(storm.intensity, std::bit_cast<std::uint32_t>(projected.front().intensity));
    ASSERT_EQ(published.strikes().size(), 1u);
    EXPECT_EQ(published.strikes().front(), before.strikes().back());
    auto clocks = before.weather_clocks();
    ++clocks.at(kFieldOwner);
    EXPECT_EQ(published.weather_clocks(), clocks);
    EXPECT_EQ(published.scent_cells(), before.scent_cells());
    EXPECT_EQ(published.soil().pages(), before.soil().pages());
    EXPECT_EQ(published.irrigation().pages(), before.irrigation().pages());
    FieldPageState decoded;
    std::string error;
    ASSERT_TRUE(FieldPageState::parse(published.serialize(), decoded, error)) << error;
    EXPECT_EQ(published.region_bytes(kFieldOwner), decoded.region_bytes(kFieldOwner));
    EXPECT_EQ(published.region_bytes({1, 0}), decoded.region_bytes({1, 0}));
}

enum class WeatherOverlayFailure {
    None,
    Consumer,
    Publication
};

void ExpectWeatherViewChannel(const Luminumbra::world::FieldPageView& view,
                              const FieldPageState& expected,
                              int channel,
                              std::int32_t x) {
    SCOPED_TRACE(channel);
    SCOPED_TRACE(x);
    std::uint16_t actual_word = 123, expected_word = 123;
    EXPECT_EQ(view.sample(channel, x, 4, actual_word),
              expected.sample(channel, x, 4, expected_word));
    EXPECT_EQ(actual_word, expected_word);
}

void ExpectWeatherViewCell(const Luminumbra::world::FieldPageView& view,
                           const FieldPageState& expected,
                           std::int32_t x) {
    for (int channel = -1; channel <= FieldPageState::kChannels; ++channel)
        ASSERT_NO_FATAL_FAILURE(ExpectWeatherViewChannel(view, expected, channel, x));
}

void CheckWeatherOverlayView() {
    FieldPageState fields;
    ASSERT_NO_FATAL_FAILURE(PopulateWeatherOverlayFields(fields, 8));
    auto ledger = ScheduledFieldLedger(
        31, {{kFieldOwner, RegionState::Active}, {{1, 0}, RegionState::Frozen}});
    const auto schedule = ledger.schedule(WorldClock(32), std::span<const Luminumbra::Vec3>{});
    const auto previous = fields.scheduled_snapshot(ledger.records(), schedule.due, 32);
    auto candidate = previous;
    candidate.copy_weather_from(fields);
    ASSERT_EQ(candidate.page_count(), 1u);
    auto expected = fields;
    const auto work = expected.step_scheduled(ledger.records(), schedule.due, 1337, 32, &fields);
    ASSERT_EQ(candidate.step_scheduled(ledger.records(), schedule.due, 1337, 32, &previous), work);
    // Empty candidate weather must override populated live weather as well.
    ASSERT_TRUE(candidate.replace_weather({}, {}, {}));
    ASSERT_TRUE(expected.replace_weather({}, {}, {}));
    const Luminumbra::world::FieldPageView view(fields, &candidate), live_view(fields, nullptr);
    EXPECT_TRUE(view.storms().empty());
    EXPECT_TRUE(view.strikes().empty());
    EXPECT_EQ(live_view.storms(), fields.storms());
    EXPECT_EQ(live_view.strikes(), fields.strikes());
    for (const std::int32_t x : {-1, 0, 20, 21, 64, 512})
        ASSERT_NO_FATAL_FAILURE(ExpectWeatherViewCell(view, expected, x));
    for (const std::uint8_t channels : {0, 1, 2, 4, 8, 16, 3, 12, 31}) {
        EXPECT_EQ(view.ambient_bytes(channels, false), expected.ambient_bytes(channels, false));
        EXPECT_EQ(view.ambient_bytes(channels, true), expected.ambient_bytes(channels, true));
        EXPECT_EQ(live_view.ambient_bytes(channels, false), fields.ambient_bytes(channels, false));
        EXPECT_EQ(live_view.ambient_bytes(channels, true), fields.ambient_bytes(channels, true));
    }
}

void CheckBoundedWeatherOverlay(std::size_t unrelated_pages, WeatherOverlayFailure failure) {
    SCOPED_TRACE(unrelated_pages);
    SCOPED_TRACE(static_cast<int>(failure));
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    std::map<RegionKey, RegionState> owners{{kFieldOwner, RegionState::Active},
                                            {{1, 0}, RegionState::Frozen}};
    // Each unrelated page sits 1536 m apart and its owner must exist for the saved world to load.
    for (std::size_t i = 0; i < unrelated_pages; ++i)
        owners.emplace(RegionKey{3 * static_cast<std::int32_t>(i + 1), 0}, RegionState::Frozen);
    auto ledger = ScheduledFieldLedger(31, owners);
    FieldPageState fields;
    ASSERT_NO_FATAL_FAILURE(PopulateWeatherOverlayFields(fields, unrelated_pages));
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    const auto schedule = ledger.schedule(WorldClock(32), std::span<const Luminumbra::Vec3>{});
    auto expected = fields;
    const auto work = expected.step_scheduled(ledger.records(), schedule.due, 1337, 32, &fields);
    ASSERT_FALSE(work.empty());
    ASSERT_NE(expected.pages(), fields.pages());
    const auto before_hashes = WeatherOverlayHashes(session);
    std::array<std::string, 3> during_hashes;
    int observations = 0;
    session.GetSimulationEventBus().subscribe([&](const auto& event) {
        if (event.topic != "weather-overlay-test")
            return;
        ++observations;
        EXPECT_EQ(session.GetRegionalFieldCandidatePageCount(), 1u);
        EXPECT_EQ(session.GetFieldPages().serialize(), fields.serialize());
        ASSERT_NO_FATAL_FAILURE(ExpectWeatherOverlaySample(session, expected, 20));
        ASSERT_NO_FATAL_FAILURE(ExpectWeatherOverlaySample(session, fields, 21));
        for (std::size_t i = 0; i < unrelated_pages; ++i)
            ASSERT_NO_FATAL_FAILURE(
                ExpectWeatherOverlaySample(session, fields, 64 * static_cast<std::int32_t>(i + 1)));
        during_hashes = WeatherOverlayHashes(session);
        if (failure == WeatherOverlayFailure::Consumer)
            throw std::runtime_error("injected weather overlay consumer fault");
        if (failure == WeatherOverlayFailure::Publication) {
            // Force completed-work admission to refuse after all consumers have run.
            auto& records = const_cast<std::map<RegionKey, ActiveRegionRecord>&>(
                session.GetActiveRegionLedger().records());
            records.erase(kFieldOwner);
        }
    });
    session.GetSimulationEventBus().publish(32, "weather-overlay-test", "");
    TickOnce(session);
    EXPECT_EQ(observations, 1);
    EXPECT_EQ(session.GetRegionalFieldCandidatePageCount(), 0u);
    if (failure != WeatherOverlayFailure::None) {
        EXPECT_TRUE(session.GetRegionSchedulerFault());
        EXPECT_EQ(session.GetFieldPages().serialize(), fields.serialize());
        EXPECT_EQ(WeatherOverlayHashes(session), before_hashes);
        EXPECT_TRUE(session.GetActiveRegionLedger().completed_work().empty());
        return;
    }
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_EQ(session.GetFieldPages().page_count(), unrelated_pages + 1);
    EXPECT_EQ(session.GetFieldPages().pages(), expected.pages());
    EXPECT_EQ(WeatherOverlayHashes(session), during_hashes);
    ASSERT_NO_FATAL_FAILURE(ExpectWeatherOverlayPublication(session, fields));
}

void AddFrozenWeatherOwners(FieldPageState& fields,
                            std::map<RegionKey, RegionState>& owners,
                            std::size_t count) {
    auto storms = fields.storms();
    auto strikes = fields.strikes();
    for (std::size_t i = 0; i < count; ++i) {
        const RegionKey owner{i == 0 ? 1 : static_cast<std::int32_t>(3 * (i + 1)), 0};
        owners.emplace(owner, RegionState::Frozen);
        ASSERT_TRUE(fields.set_weather_age(owner, 19));
        const float x = static_cast<float>(owner.x) * 512.0f + 256.0f;
        for (std::size_t j = 0; j < FieldPageState::kMaxOwnerStorms; ++j)
            storms.push_back(SessionStorm(
                owner, x, 256, 0, 0, 10, static_cast<std::uint32_t>(100 + 16 * i + j)));
        strikes.push_back({owner,
                           31,
                           std::bit_cast<std::uint32_t>(x),
                           std::bit_cast<std::uint32_t>(256.0f),
                           std::bit_cast<std::uint32_t>(0.5f),
                           static_cast<std::uint32_t>(100 + 16 * i),
                           0});
    }
    ASSERT_TRUE(fields.replace_weather(storms, strikes));
}

void ExpectFrozenWeatherUnchanged(const GameSession& session,
                                  const FieldPageState& before,
                                  const std::map<RegionKey, RegionState>& owners) {
    for (const auto& [owner, state] : owners) {
        if (state != RegionState::Frozen)
            continue;
        EXPECT_EQ(session.GetFieldPages().region_bytes(owner), before.region_bytes(owner));
        const Luminumbra::Vec3 center{static_cast<float>(owner.x) * 512.0f + 256.0f, 0, 256};
        EXPECT_EQ(session.GetWeatherSystem()->storm_candidates_at(center),
                  FieldPageState::kMaxOwnerStorms);
        EXPECT_FLOAT_EQ(session.GetWeatherSystem()->SampleAt(center).storm_intensity, 1.0f);
    }
}

void CheckFrozenWeatherWork(std::size_t frozen_owners,
                            int work_kind,
                            bool shared_page = false,
                            std::array<std::size_t, 3>* measured = nullptr) {
    SCOPED_TRACE(shared_page);
    SCOPED_TRACE(frozen_owners);
    SCOPED_TRACE(work_kind);
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    std::map<RegionKey, RegionState> owners{{kFieldOwner, RegionState::Active}};
    FieldPageState fields;
    if (work_kind == 0) {
        ASSERT_TRUE(fields.set_soil({0, 0}, 100));
    }
    if (work_kind == 1) {
        ASSERT_TRUE(fields.set_weather_age(kFieldOwner, 7));
    }
    if (work_kind == 2 || work_kind == 4) {
        ASSERT_TRUE(fields.replace_weather({SessionStorm(kFieldOwner, 256, 256)}, {}));
    }
    if (work_kind == 3) {
        ASSERT_TRUE(fields.replace_weather({},
                                           {{kFieldOwner,
                                             31,
                                             std::bit_cast<std::uint32_t>(256.0f),
                                             std::bit_cast<std::uint32_t>(256.0f),
                                             std::bit_cast<std::uint32_t>(0.5f),
                                             99,
                                             0}}));
    }
    if (work_kind == 4) {
        AddWeatherOverlayCell(fields, 4, {1024, 64512, 32768, 32768, 16384});
    }
    ASSERT_NO_FATAL_FAILURE(AddFrozenWeatherOwners(fields, owners, frozen_owners));
    if (shared_page && frozen_owners != 0)
        AddWeatherOverlayCell(fields, 21, {1024, 64512, 32768, 32768, 16384});
    const auto ledger = ScheduledFieldLedger(31, owners);
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    EXPECT_EQ(session.GetWeatherSystem()->live_strike_count(), 0);
    EXPECT_TRUE(session.GetWeatherSystem()->StrikeSchedule().empty());
    int observations = 0;
    session.GetSimulationEventBus().subscribe([&](const auto& event) {
        if (event.topic != "weather-work-test")
            return;
        ++observations;
        EXPECT_EQ(session.GetRegionalFieldCandidatePageCount(), work_kind == 4 ? 1u : 0u);
        EXPECT_EQ(session.GetRegionalWeatherProjectionCount(),
                  work_kind == 2 || work_kind == 4 ? 1u : 0u);
        if (work_kind == 0) {
            EXPECT_EQ(session.GetRegionalWeatherCandidateRecordCount(), 0u);
        }
        const auto staged = session.GetRegionalWeatherCandidateRecordCount();
        EXPECT_LE(staged, 1u);
        if (measured) {
            *measured = {staged,
                         session.GetRegionalWeatherScanCount(),
                         session.GetRegionalWeatherProjectionVisitCount()};
        }
        EXPECT_EQ(session.GetFieldPages().serialize(), fields.serialize());
    });
    session.GetSimulationEventBus().publish(32, "weather-work-test", "");
    TickOnce(session);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_EQ(observations, 1);
    const auto* weather = session.GetWeatherSystem();
    EXPECT_EQ(weather->last_tick(), 32u);
    EXPECT_EQ(weather->AnchorCategory(),
              weather->SampleAt(session.GetMetadata().spawnPoint).category);
    if (work_kind == 0) {
        EXPECT_EQ(session.GetFieldPages().serialize(), fields.serialize());
        EXPECT_TRUE(session.GetActiveRegionLedger().completed_work().empty());
    } else {
        EXPECT_EQ(session.GetFieldPages().weather_clocks().at(kFieldOwner),
                  work_kind == 1 ? 8u : 1u);
        EXPECT_EQ(weather->StrikeSchedule().size(), frozen_owners);
        EXPECT_EQ(weather->live_strike_count(), static_cast<int>(frozen_owners));
    }
    ASSERT_NO_FATAL_FAILURE(ExpectFrozenWeatherUnchanged(session, fields, owners));
}

void ExpectMissingWeatherSamples(GameSession& expected, GameSession& actual) {
    for (const Luminumbra::Vec3 position :
         {Luminumbra::Vec3{-7000, 0, 3100}, Luminumbra::Vec3{8134, 0, -9111}}) {
        EXPECT_EQ(actual.GetWindFieldSystem()->SampleWind(position),
                  expected.GetWindFieldSystem()->SampleWind(position));
        const auto sample = actual.GetWeatherSystem()->SampleAt(position);
        const auto reference = expected.GetWeatherSystem()->SampleAt(position);
        EXPECT_EQ(sample.category, reference.category);
        EXPECT_EQ(sample.wind, reference.wind);
        EXPECT_FLOAT_EQ(sample.precip_intensity, reference.precip_intensity);
        EXPECT_FLOAT_EQ(actual.GetAetherFieldSystem()->SampleAether(position),
                        expected.GetAetherFieldSystem()->SampleAether(position));
    }
}

void CheckWeatherSeedAdoption(const std::string& seed) {
    SCOPED_TRACE(seed);
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original, reopened;
    EnableSnapshotSession(original, root, jobs);
    EnableSnapshotSession(reopened, root, jobs);
    ASSERT_TRUE(original.CreateWorld("Weather seed", seed, "default"));
    FieldPageState fields;
    ASSERT_TRUE(fields.set_weather_age(kFieldOwner, 44));
    ASSERT_NO_FATAL_FAILURE(LoadSessionFieldInput(original, root.path() / "seed-input", fields));
    ASSERT_TRUE(original.SaveWorld());
    ASSERT_TRUE(reopened.CreateTransientWorld("Previous seed", "7331", "default"));
    ASSERT_TRUE(reopened.LoadWorld(original.GetMetadata().worldId));
    original.SetReplicatedSimulationAnchors({});
    reopened.SetReplicatedSimulationAnchors({});
    ASSERT_NO_FATAL_FAILURE(ExpectMissingWeatherSamples(original, reopened));
    for (int tick = 0; tick < 46; ++tick) {
        TickOnce(original);
        TickOnce(reopened);
        ASSERT_NO_FATAL_FAILURE(ExpectMissingWeatherSamples(original, reopened));
        ASSERT_NO_FATAL_FAILURE(ExpectWeatherContinuation(original, reopened));
    }
}

void CheckSynchronizedWeatherSpawns() {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    std::map<RegionKey, RegionState> owners;
    FieldPageState fields;
    for (std::int32_t x = -16; x < 16; ++x) {
        const RegionKey owner{x, -1};
        owners.emplace(owner, RegionState::Active);
        ASSERT_TRUE(fields.set_weather_age(owner, 44));
    }
    const auto ledger = ScheduledFieldLedger(1003, owners);
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    TickOnce(session);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    const auto& storms = session.GetFieldPages().storms();
    ASSERT_GT(storms.size(), 1u);
    std::set<std::pair<std::uint64_t, std::uint32_t>> identities;
    for (const auto& storm : storms) {
        EXPECT_EQ(storm.spawn_tick, 1004u);
        EXPECT_TRUE(identities.emplace(storm.spawn_tick, storm.seed_salt).second);
    }
    FieldPageState decoded;
    std::string error;
    ASSERT_TRUE(FieldPageState::parse(session.GetFieldPages().serialize(), decoded, error))
        << error;
    EXPECT_EQ(decoded.storms(), storms);
}

void CheckTransferredStormReservesSpawnIdentity() {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession control, session;
    const RegionKey target{0, -1};
    std::map<RegionKey, RegionState> owners{{target, RegionState::Active}};
    FieldPageState fields;
    ASSERT_TRUE(fields.set_weather_age(target, 44));
    const auto control_ledger = ScheduledFieldLedger(1003, owners);
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(control, root, jobs, control_ledger, fields));
    TickOnce(control);
    ASSERT_FALSE(control.GetRegionSchedulerFault());
    ASSERT_EQ(control.GetFieldPages().storms().size(), 1u);
    const auto drawn = control.GetFieldPages().storms().front();
    // Select a neighbouring source whose interior is closed at the common boundary.
    std::optional<RegionKey> source;
    for (std::int32_t dx = -1; dx <= 1; ++dx) {
        for (std::int32_t dz = -1; dz <= 1; ++dz) {
            const RegionKey neighbour{target.x + dx, target.z + dz};
            if (neighbour == target) {
                continue;
            }
            owners.emplace(neighbour, RegionState::Reduced);
            if (!source && ActiveRegionLedger::phase(neighbour, 2) != 0) {
                source = neighbour;
            }
        }
    }
    ASSERT_TRUE(source.has_value());
    if (!source) {
        return;
    }
    const auto crossing = [](std::int32_t from, std::int32_t to) {
        const float low = static_cast<float>(to) * 512.0f;
        return from < to   ? std::pair{low - 0.25f, 30.0f}
               : from > to ? std::pair{low + 512.25f, -30.0f}
                           : std::pair{low + 256.0f, 0.0f};
    };
    const auto [x, vx] = crossing(source->x, target.x);
    const auto [z, vz] = crossing(source->z, target.z);
    auto incoming = SessionStorm(*source, x, z, vx, vz);
    incoming.spawn_tick = drawn.spawn_tick;
    incoming.seed_salt = drawn.seed_salt;
    ASSERT_TRUE(fields.replace_weather({incoming}, {}));
    const auto ledger = ScheduledFieldLedger(1003, owners);
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    TickOnce(session);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    ASSERT_EQ(session.GetFieldPages().storms().size(), 1u);
    const auto& moved = session.GetFieldPages().storms().front();
    EXPECT_EQ(moved.owner, target);
    EXPECT_EQ(moved.spawn_tick, drawn.spawn_tick);
    EXPECT_EQ(moved.seed_salt, drawn.seed_salt);
    EXPECT_EQ(moved.active_age, incoming.active_age);
}

void CheckWeatherWithoutAmbientPages(int authority, bool clock) {
    SCOPED_TRACE(clock);
    SCOPED_TRACE(authority);
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    const auto ledger = ScheduledFieldLedger(31, {{kFieldOwner, RegionState::Active}});
    FieldPageState fields;
    if (authority == 0) {
        ASSERT_TRUE(fields.set_scent({0, 0}, {std::bit_cast<std::uint64_t>(1.0), 0, 0, 0}));
    } else if (authority == 1) {
        ASSERT_TRUE(fields.set_soil({0, 0}, 100));
    } else {
        ASSERT_TRUE(fields.set_irrigation({0, 0}, 200));
    }
    if (clock) {
        ASSERT_TRUE(fields.set_weather_age(kFieldOwner, 7));
    }
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    TickOnce(session);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_EQ(session.GetFieldPages().page_count(), 0u);
    if (clock) {
        EXPECT_EQ(session.GetFieldPages().weather_clocks(),
                  (std::map<RegionKey, std::uint64_t>{{kFieldOwner, 8}}));
    } else {
        EXPECT_TRUE(session.GetFieldPages().weather_clocks().empty());
    }
    EXPECT_EQ(session.GetFieldPages().scent_cells(), fields.scent_cells());
    EXPECT_EQ(session.GetFieldPages().soil().pages(), fields.soil().pages());
    EXPECT_EQ(session.GetFieldPages().irrigation().pages(), fields.irrigation().pages());
}

void CheckUnrelatedFieldOwnerWeather(int authority) {
    SCOPED_TRACE(authority);
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    const RegionKey weather_owner{1, 0};
    FieldPageState fields;
    if (authority == 0) {
        ASSERT_TRUE(fields.set_scent({0, 0}, {std::bit_cast<std::uint64_t>(1.0), 0, 0, 0}));
    } else if (authority == 1) {
        ASSERT_TRUE(fields.set_soil({0, 0}, 100));
    } else {
        ASSERT_TRUE(fields.set_irrigation({0, 0}, 200));
    }
    ASSERT_TRUE(fields.set_weather_age(weather_owner, 7));
    const auto ledger = ScheduledFieldLedger(
        31, {{kFieldOwner, RegionState::Active}, {weather_owner, RegionState::Active}});
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    for (int i = 0; i < 2; ++i) {
        TickOnce(session);
        ASSERT_FALSE(session.GetRegionSchedulerFault());
        EXPECT_EQ(session.GetFieldPages().region_bytes(kFieldOwner),
                  fields.region_bytes(kFieldOwner));
        EXPECT_FALSE(session.GetFieldPages().owns_weather(kFieldOwner));
        EXPECT_FALSE(session.GetActiveRegionLedger().completed_work().contains(kFieldOwner));
        EXPECT_EQ(session.GetFieldPages().weather_clocks().at(weather_owner),
                  static_cast<std::uint64_t>(8 + i));
    }
}

void CheckWeatherOwnerWorkBound(int kind) {
    SCOPED_TRACE(kind);
    std::array<std::size_t, 3> baseline{};
    ASSERT_NO_FATAL_FAILURE(CheckFrozenWeatherWork(0, kind, false, &baseline));
    for (const std::size_t owners : {std::size_t{16}, std::size_t{128}, std::size_t{1024}}) {
        std::array<std::size_t, 3> measured{};
        ASSERT_NO_FATAL_FAILURE(CheckFrozenWeatherWork(owners, kind, false, &measured));
        EXPECT_EQ(measured, baseline);
    }
}

void ExpectNewWorldRegionalWeather(GameSession& session, GameSession& restored) {
    const Luminumbra::Vec3 position{108, 0, 108};
    const auto actual = session.GetWeatherSystem()->SampleAt(position);
    const auto expected = restored.GetWeatherSystem()->SampleAt(position);
    EXPECT_EQ(actual.category, expected.category);
    EXPECT_EQ(actual.precip_intensity, expected.precip_intensity);
    EXPECT_EQ(actual.storm_intensity, expected.storm_intensity);
    EXPECT_EQ(actual.wind, expected.wind);
    EXPECT_EQ(session.GetWindFieldSystem()->SampleWind(position),
              restored.GetWindFieldSystem()->SampleWind(position));
    EXPECT_EQ(session.GetAetherFieldSystem()->SampleAether(position),
              restored.GetAetherFieldSystem()->SampleAether(position));
    const auto& storms = session.GetFieldPages().storms();
    const auto& projected = session.GetWeatherSystem()->StormCells();
    ASSERT_EQ(storms.size(), 1u);
    ASSERT_EQ(projected.size(), storms.size());
    EXPECT_EQ(storms.front().center_x,
              std::bit_cast<std::uint32_t>(projected.front().center_world.x));
    EXPECT_EQ(storms.front().center_z,
              std::bit_cast<std::uint32_t>(projected.front().center_world.y));
    EXPECT_EQ(storms.front().intensity, std::bit_cast<std::uint32_t>(projected.front().intensity));
}

void CheckNewWorldRegionalBinding() {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, reopened;
    EnableSnapshotSession(session, root, jobs);
    ASSERT_TRUE(session.CreateWorld("Regional weather binding", "1337", "default"));
    session.SetReplicatedSimulationAnchors({});
    session.PinActiveRegion(kFieldOwner, true);
    ASSERT_TRUE(session.GetFieldPages().empty());
    FieldPageState fields;
    AddWeatherOverlayCell(fields, 4, {1024, 64512, 32768, 32768, 16384});
    ASSERT_TRUE(fields.replace_weather({SessionStorm(kFieldOwner, 108, 108)}, {}));
    // No runtime ambient-page producer exists yet. Introduce first authority at an event
    // boundary, without loading the original session or invoking its restore path.
    int adoptions = 0;
    session.GetSimulationEventBus().subscribe([&](const auto& event) {
        if (event.topic == "first-regional-authority") {
            const_cast<FieldPageState&>(session.GetFieldPages()) = fields;
            ++adoptions;
        }
    });
    session.GetSimulationEventBus().publish(1, "first-regional-authority", "");
    TickOnce(session);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_EQ(adoptions, 1);
    ASSERT_NO_FATAL_FAILURE(ReopenSessionWeather(session, reopened, root, jobs));
    ASSERT_NO_FATAL_FAILURE(ExpectNewWorldRegionalWeather(session, reopened));
    TickOnce(session);
    TickOnce(reopened);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    ASSERT_NO_FATAL_FAILURE(ExpectWeatherContinuation(session, reopened));
    ASSERT_NO_FATAL_FAILURE(ExpectNewWorldRegionalWeather(session, reopened));
    TickOnce(session);
    TickOnce(reopened);
    ASSERT_NO_FATAL_FAILURE(ExpectWeatherContinuation(session, reopened));
    ASSERT_NO_FATAL_FAILURE(ExpectNewWorldRegionalWeather(session, reopened));
}

void FillGlobalStormCapacity(std::vector<RegionalStorm>& storms,
                             std::map<RegionKey, RegionState>& owners) {
    const auto participating = storms.size();
    for (std::size_t i = participating; i < FieldPageState::kMaxStorms; ++i) {
        const RegionKey owner{
            static_cast<std::int32_t>(100 + (i - participating) / FieldPageState::kMaxOwnerStorms),
            0};
        owners.emplace(owner, RegionState::Frozen);
        storms.push_back(SessionStorm(owner,
                                      static_cast<float>(owner.x) * 512.0f + 256.0f,
                                      256,
                                      0,
                                      0,
                                      10,
                                      static_cast<std::uint32_t>(100 + i)));
    }
}

void CheckGlobalStormSpawnCapacity(bool expires) {
    SCOPED_TRACE(expires);
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, control;
    const RegionKey owner{0, -1};
    std::map<RegionKey, RegionState> owners{{owner, RegionState::Active}};
    FieldPageState fields, control_fields;
    const auto storm = SessionStorm(owner, 256, -256, 0, 0, expires ? 239 : 10);
    ASSERT_TRUE(control_fields.replace_weather({storm}, {}));
    ASSERT_TRUE(control_fields.set_weather_age(owner, 44));
    std::vector<RegionalStorm> storms{storm};
    FillGlobalStormCapacity(storms, owners);
    ASSERT_TRUE(fields.replace_weather(storms, {}));
    ASSERT_TRUE(fields.set_weather_age(owner, 44));
    const auto ledger = ScheduledFieldLedger(1003, owners);
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(control, root, jobs, ledger, control_fields));
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    TickOnce(control);
    ASSERT_FALSE(control.GetRegionSchedulerFault());
    ASSERT_FALSE(control.GetFieldPages().storms().empty());
    const auto& drawn = control.GetFieldPages().storms().back();
    ASSERT_EQ(drawn.spawn_tick, 1004u); // Observe the draw rather than duplicating its hash.
    TickOnce(session);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_EQ(session.GetFieldPages().storms().size(), FieldPageState::kMaxStorms);
    EXPECT_EQ(session.GetFieldPages().weather_clocks().at(owner), 45u);
    auto frozen_before = fields.storms();
    auto frozen_after = session.GetFieldPages().storms();
    std::erase_if(frozen_before, [owner](const auto& value) { return value.owner == owner; });
    std::erase_if(frozen_after, [owner](const auto& value) { return value.owner == owner; });
    EXPECT_EQ(frozen_after, frozen_before);
    const auto indices = session.GetFieldPages().storm_indices(owner);
    ASSERT_EQ(indices.size(), 1u);
    const auto& survivor = session.GetFieldPages().storms()[indices.front()];
    if (expires) {
        EXPECT_EQ(survivor, drawn);
    } else {
        EXPECT_EQ(survivor.spawn_tick, storm.spawn_tick);
        EXPECT_EQ(survivor.active_age, storm.active_age + 1);
    }
}

void CheckGlobalStormTransferCapacity() {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    const RegionKey source{0, -1}, target{1, -1};
    std::map<RegionKey, RegionState> owners{{source, RegionState::Reduced},
                                            {target, RegionState::Reduced}};
    const auto incoming = SessionStorm(source, 511.75f, -256, 30, 0);
    std::vector<RegionalStorm> storms{incoming};
    FillGlobalStormCapacity(storms, owners);
    FieldPageState fields;
    ASSERT_TRUE(fields.replace_weather(storms, {}));
    const auto ledger = ScheduledFieldLedger(1003, owners);
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    int observations = 0;
    session.GetSimulationEventBus().subscribe([&](const auto& event) {
        if (event.topic == "weather-transfer-work") {
            ++observations;
            EXPECT_LE(session.GetRegionalWeatherCandidateRecordCount(), 2u);
            EXPECT_LT(session.GetRegionalWeatherScanCount(), 64u);
            EXPECT_LT(session.GetRegionalWeatherProjectionVisitCount(), 64u);
        }
    });
    session.GetSimulationEventBus().publish(1004, "weather-transfer-work", "");
    TickOnce(session);
    EXPECT_EQ(observations, 1);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    ASSERT_EQ(session.GetFieldPages().storms().size(), FieldPageState::kMaxStorms);
    const auto& moved = session.GetFieldPages().storms().front();
    auto expected = incoming;
    expected.owner = target;
    expected.center_x = std::bit_cast<std::uint32_t>(512.75f);
    EXPECT_EQ(moved, expected);
    EXPECT_TRUE(std::equal(session.GetFieldPages().storms().begin() + 1,
                           session.GetFieldPages().storms().end(),
                           fields.storms().begin() + 1));
}

void FillGlobalStrikeCapacity(std::vector<Luminumbra::world::RegionalStrike>& strikes,
                              std::map<RegionKey, RegionState>& owners) {
    const auto participating = strikes.size();
    for (std::size_t i = participating; i < FieldPageState::kMaxStrikes; ++i) {
        const RegionKey owner{
            static_cast<std::int32_t>(100 + (i - participating) / FieldPageState::kMaxOwnerStrikes),
            0};
        owners.emplace(owner, RegionState::Frozen);
        strikes.push_back(
            {owner,
             1003,
             std::bit_cast<std::uint32_t>(static_cast<float>(owner.x) * 512.0f + 256.0f),
             std::bit_cast<std::uint32_t>(256.0f),
             std::bit_cast<std::uint32_t>(0.5f),
             static_cast<std::uint32_t>(100 + i),
             0});
    }
}

void CheckGlobalStrikeCapacity(bool expires) {
    SCOPED_TRACE(expires);
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, control;
    const RegionKey source{0, -1}, target{1, -1};
    std::map<RegionKey, RegionState> owners{{source, RegionState::Active},
                                            {target, RegionState::Reduced}};
    std::vector<Luminumbra::world::RegionalStrike> strikes;
    if (expires) {
        strikes.push_back({source,
                           1003,
                           std::bit_cast<std::uint32_t>(256.0f),
                           std::bit_cast<std::uint32_t>(-256.0f),
                           std::bit_cast<std::uint32_t>(0.5f),
                           99,
                           0});
    }
    FieldPageState fields, control_fields;
    const auto storm = SessionStorm(source, 500, -256, 0, 0, 53, 4);
    ASSERT_TRUE(control_fields.replace_weather({storm}, strikes));
    ASSERT_TRUE(control_fields.set_weather_age(source, 7));
    FillGlobalStrikeCapacity(strikes, owners);
    ASSERT_TRUE(fields.replace_weather({storm}, strikes));
    ASSERT_TRUE(fields.set_weather_age(source, 7));
    const auto ledger = ScheduledFieldLedger(1003, owners);
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(control, root, jobs, ledger, control_fields));
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    TickOnce(control);
    ASSERT_FALSE(control.GetRegionSchedulerFault());
    ASSERT_EQ(control.GetFieldPages().strikes().size(), 1u); // Observe a successful strike draw.
    TickOnce(session);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_EQ(session.GetFieldPages().storms(), control.GetFieldPages().storms());
    EXPECT_EQ(session.GetFieldPages().weather_clocks().at(source), 8u);
    auto expected = fields.strikes();
    if (expires) {
        std::erase_if(expected, [source](const auto& value) { return value.owner == source; });
        expected.push_back(control.GetFieldPages().strikes().front());
    }
    EXPECT_EQ(session.GetFieldPages().strikes(), expected);
    EXPECT_EQ(session.GetFieldPages().strikes().size(), FieldPageState::kMaxStrikes);
}

} // namespace

TEST(GameSessionHeadlessWorldTest, RegionalWeatherBindsFirstAuthorityWithoutReload) {
    ASSERT_NO_FATAL_FAILURE(CheckNewWorldRegionalBinding());
}

TEST(GameSessionHeadlessWorldTest, RegionalWeatherLeavesUnrelatedFieldOwnersUnchanged) {
    for (const int authority : {0, 1, 2}) {
        ASSERT_NO_FATAL_FAILURE(CheckUnrelatedFieldOwnerWeather(authority));
    }
}

TEST(GameSessionHeadlessWorldTest, RegionalWeatherSuppressesSpawnsAtGlobalCapacityAndReusesExpiry) {
    for (const bool expires : {false, true}) {
        ASSERT_NO_FATAL_FAILURE(CheckGlobalStormSpawnCapacity(expires));
    }
}

TEST(GameSessionHeadlessWorldTest, RegionalWeatherTransfersAtGlobalStormCapacity) {
    ASSERT_NO_FATAL_FAILURE(CheckGlobalStormTransferCapacity());
}

TEST(GameSessionHeadlessWorldTest,
     RegionalWeatherSuppressesStrikesAtGlobalCapacityAndReusesExpiry) {
    for (const bool expires : {false, true}) {
        ASSERT_NO_FATAL_FAILURE(CheckGlobalStrikeCapacity(expires));
    }
}

TEST(GameSessionHeadlessWorldTest,
     RegionalWeatherViewPreservesEveryChannelAndCanonicalAmbientBytes) {
    ASSERT_NO_FATAL_FAILURE(CheckWeatherOverlayView());
}

TEST(GameSessionHeadlessWorldTest, RegionalWeatherOverlaysOneSteppedPageOnManyLivePages) {
    for (const std::size_t unrelated_pages : {std::size_t{8}, std::size_t{48}})
        ASSERT_NO_FATAL_FAILURE(
            CheckBoundedWeatherOverlay(unrelated_pages, WeatherOverlayFailure::None));
}

TEST(GameSessionHeadlessWorldTest, RegionalWeatherOverlayDiscardsConsumerAndPublicationFailures) {
    for (const auto failure : {WeatherOverlayFailure::Consumer, WeatherOverlayFailure::Publication})
        ASSERT_NO_FATAL_FAILURE(CheckBoundedWeatherOverlay(8, failure));
}

TEST(GameSessionHeadlessWorldTest, RegionalWeatherWithoutAmbientPagesRequiresWeatherAuthority) {
    for (const int authority : {0, 1, 2})
        for (const bool clock : {false, true})
            ASSERT_NO_FATAL_FAILURE(CheckWeatherWithoutAmbientPages(authority, clock));
}

TEST(GameSessionHeadlessWorldTest, RegionalWeatherSpawnAndStrikeContinueAcrossReopen) {
    for (const bool frozen_target : {false, true}) {
        ASSERT_NO_FATAL_FAILURE(CheckWeatherReopenAndStrike(frozen_target));
    }
}

TEST(GameSessionHeadlessWorldTest, RegionalWeatherHandoverUsesCommonPhaseWithoutDueSource) {
    ASSERT_NO_FATAL_FAILURE(CheckWeatherHandover());
}

TEST(GameSessionHeadlessWorldTest, RegionalWeatherHandoverTracksNewAndAwakenedTargets) {
    for (const bool frozen : {false, true}) {
        ASSERT_NO_FATAL_FAILURE(CheckWeatherBoundaryTargetAdmission(frozen));
    }
}

TEST(GameSessionHeadlessWorldTest, RegionalWeatherCornerRequiresBothCardinalOwners) {
    for (const int blocked : {0, 1, 2, 3}) {
        ASSERT_NO_FATAL_FAILURE(CheckWeatherCorner(blocked));
    }
}

TEST(GameSessionHeadlessWorldTest, RegionalWeatherExpiresOnActiveAgeAndLeavesFrozenStateExact) {
    for (const bool frozen : {false, true}) {
        ASSERT_NO_FATAL_FAILURE(CheckWeatherFrozenAndExpiry(frozen));
    }
}

TEST(GameSessionHeadlessWorldTest, RegionalWeatherFailurePreservesAuthorityAndAmbientHashes) {
    for (const bool late_failure : {false, true}) {
        ASSERT_NO_FATAL_FAILURE(CheckWeatherFailure(late_failure));
    }
}

TEST(GameSessionHeadlessWorldTest, RegionalWeatherSamplesCentreOwnedCellsAcrossCornerAndReopen) {
    ASSERT_NO_FATAL_FAILURE(CheckWeatherCellOwnership());
}

TEST(GameSessionHeadlessWorldTest, RegionalWeatherDueMotionUsesCurrentPagesAndClampsBlockedCorner) {
    ASSERT_NO_FATAL_FAILURE(CheckWeatherBlockedDueMotion());
}

TEST(GameSessionHeadlessWorldTest, RegionalWeatherSuppressesSpawnsAtOwnerCapacity) {
    for (const std::size_t count : {std::size_t{15}, std::size_t{16}})
        ASSERT_NO_FATAL_FAILURE(CheckWeatherSpawnCapacity(count, false));
    ASSERT_NO_FATAL_FAILURE(CheckWeatherSpawnCapacity(16, true));
}

TEST(GameSessionHeadlessWorldTest, RegionalWeatherBlocksTransfersAtOwnerCapacity) {
    for (const std::size_t count : {std::size_t{15}, std::size_t{16}})
        ASSERT_NO_FATAL_FAILURE(CheckWeatherTransferCapacity(count));
    ASSERT_NO_FATAL_FAILURE(CheckWeatherTransferCapacity(16, true));
    ASSERT_NO_FATAL_FAILURE(CheckWeatherTransferCapacity(15, false, 2));
}

TEST(GameSessionHeadlessWorldTest, RegionalWeatherIdleTicksBorrowGrowingFrozenAuthority) {
    for (const std::size_t owners : {std::size_t{0}, std::size_t{16}, std::size_t{128}})
        for (const bool shared_page : {false, true})
            ASSERT_NO_FATAL_FAILURE(CheckFrozenWeatherWork(owners, 0, shared_page));
}

TEST(GameSessionHeadlessWorldTest, RegionalWeatherProjectionWorkStaysFixedWithFrozenAuthority) {
    for (const std::size_t owners : {std::size_t{0}, std::size_t{16}, std::size_t{128}})
        for (const int work : {1, 2, 3, 4})
            ASSERT_NO_FATAL_FAILURE(CheckFrozenWeatherWork(owners, work));
}

TEST(GameSessionHeadlessWorldTest, RegionalWeatherOwnerWorkIsIndependentOfFrozenAuthority) {
    for (const int kind : {1, 2, 3, 4}) {
        ASSERT_NO_FATAL_FAILURE(CheckWeatherOwnerWorkBound(kind));
    }
}

TEST(GameSessionHeadlessWorldTest, RegionalWeatherResolvesNumericAndTextSeedsAcrossAdoption) {
    for (const auto* seed : {"1337", "regional-weather"})
        ASSERT_NO_FATAL_FAILURE(CheckWeatherSeedAdoption(seed));
}

TEST(GameSessionHeadlessWorldTest, RegionalWeatherAdmitsUniqueSynchronizedSpawns) {
    ASSERT_NO_FATAL_FAILURE(CheckSynchronizedWeatherSpawns());
}

TEST(GameSessionHeadlessWorldTest, RegionalWeatherTransferredStormReservesSpawnIdentity) {
    ASSERT_NO_FATAL_FAILURE(CheckTransferredStormReservesSpawnIdentity());
}

TEST(GameSessionHeadlessWorldTest, RegionalWeatherTransfersWithoutUnrelatedDueAuthority) {
    for (const bool unrelated_weather : {false, true}) {
        for (const int page_kind : {0, 1, 2}) {
            ASSERT_NO_FATAL_FAILURE(CheckWeatherWithoutDueAuthority(unrelated_weather, page_kind));
        }
    }
}

TEST(GameSessionHeadlessWorldTest, RegionalWeatherSkipsCandidatesWithoutDueAuthority) {
    ASSERT_NO_FATAL_FAILURE(CheckFrozenWeatherWork(0, 0));
}

TEST(GameSessionHeadlessWorldTest, RegionalWeatherWaitsForDestinationDueMembership) {
    for (const bool unrelated_weather : {false, true}) {
        for (const bool active_source : {false, true}) {
            ASSERT_NO_FATAL_FAILURE(
                CheckWeatherWaitsForDestination(unrelated_weather, active_source));
        }
    }
}

TEST(GameSessionHeadlessWorldTest, RegionalWeatherStationaryAgeTickRetainsProjectionNodes) {
    ASSERT_NO_FATAL_FAILURE(CheckStationaryWeatherProjection());
}

namespace {

std::vector<std::uint64_t> ScentBits(const luminumbra::ai::ScentField& field) {
    std::vector<std::uint64_t> bits;
    for (int ch = 0; ch < field.channels(); ++ch)
        for (int x = 0; x < field.width(); ++x)
            for (int z = 0; z < field.height(); ++z)
                bits.push_back(std::bit_cast<std::uint64_t>(field.Sample(ch, x, z)));
    return bits;
}

std::vector<std::uint64_t> SessionScentBits(const GameSession& session) {
    return ScentBits(*session.GetScentField());
}

entt::entity AddSessionScentParticipant(GameSession& session, int kind, float x) {
    namespace C = Luminumbra::Components;
    auto& registry = session.GetRegistry();
    const auto entity = registry.create();
    registry.emplace<C::TransformComponent>(entity).position = {x, 0, 1};
    if (kind == 0) {
        auto& emitter = registry.emplace<C::SensableComponent>(entity);
        emitter.scent_channel = 0;
        emitter.scent_deposit = 8;
    } else if (kind == 1) {
        registry.emplace<C::ScentSenseComponent>(entity).channel = 0;
        registry.emplace<C::LocomotionProfile>(entity);
        registry.emplace<C::LocomotionIntentComponent>(entity);
    } else {
        auto& ant = registry.emplace<C::ForagerComponent>(entity);
        ant.cell_x = session.ScentWorldToCellX(x);
        ant.cell_z = session.ScentWorldToCellZ(1);
        ant.home_x = ant.cell_x + 4;
        ant.home_z = ant.cell_z;
        ant.carrying_food = true;
    }
    return entity;
}

void CheckSpawnScentConsumers(GameSession& session) {
    namespace C = Luminumbra::Components;
    const auto spawn = session.GetMetadata().spawnPoint;
    const int side = session.ScentFieldCells();
    EXPECT_EQ(session.ScentCellToWorldX(0), std::floor(spawn.x) - 64.0f);
    EXPECT_EQ(session.ScentCellToWorldZ(0), std::floor(spawn.z) - 64.0f);
    const auto creature = ClockTestCreature(session.GetRegistry());
    auto& position = session.GetRegistry().get<C::TransformComponent>(creature).position;
    position = spawn + Luminumbra::Vec3(3, 0, -3);
    EXPECT_TRUE(session.GetScentField()->in_bounds(session.ScentWorldToCellX(position.x),
                                                   session.ScentWorldToCellZ(position.z)));
    session.GetRegistry().destroy(creature);
    // Same nest clamp and offsets as the client colony spawner.
    const int nest_x = std::clamp(session.ScentWorldToCellX(spawn.x), 0, side - 1);
    const int nest_z = std::clamp(session.ScentWorldToCellZ(spawn.z) + 6, 0, side - 1);
    EXPECT_EQ(nest_x, side / 2 + std::lround(spawn.x - std::floor(spawn.x)));
    EXPECT_EQ(nest_z, side / 2 + 6 + std::lround(spawn.z - std::floor(spawn.z)));
    EXPECT_LE(std::abs(session.ScentCellToWorldX(nest_x) - spawn.x), 1.0f);
    EXPECT_LE(std::abs(session.ScentCellToWorldZ(nest_z) - spawn.z - 6), 1.0f);
}

void CheckSessionScentProjection(const GameSession& session) {
    const auto& fields = session.GetFieldPages();
    std::vector<std::uint64_t> expected;
    for (int ch = 0; ch < 4; ++ch) {
        for (int x = 0; x < session.ScentFieldCells(); ++x) {
            for (int z = 0; z < session.ScentFieldCells(); ++z) {
                const Luminumbra::world::ScentCellKey key{
                    static_cast<std::int32_t>(session.ScentCellToWorldX(x)),
                    static_cast<std::int32_t>(session.ScentCellToWorldZ(z))};
                expected.push_back(fields.scent_at(key)[static_cast<std::size_t>(ch)]);
            }
        }
    }
    EXPECT_EQ(SessionScentBits(session), expected);
}

void CheckNewWorldScentSpawnMapping(bool active_regions) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    EnableSnapshotSession(session, root, jobs);
    session.SetActiveRegionsEnabled(active_regions);
    ASSERT_TRUE(session.CreateWorld("Scent spawn mapping", "1337", "default"));
    if (active_regions) {
        ASSERT_NO_FATAL_FAILURE(CheckSpawnScentConsumers(session));
        ASSERT_NO_FATAL_FAILURE(CheckSessionScentProjection(session));
    } else {
        session.GetScentField()->Deposit(0, 10, 10, 1.0);
        const auto bits = SessionScentBits(session);
        const auto spawn =
            session.GetMetadata().spawnPoint + Luminumbra::Vec3(1024.75f, 0, -512.25f);
        session.SetSpawnPoint(spawn);
        EXPECT_EQ(SessionScentBits(session), bits);
        EXPECT_EQ(session.ScentCellToWorldX(0), spawn.x - 64.0f);
        EXPECT_EQ(session.ScentCellToWorldZ(0), spawn.z - 64.0f);
        EXPECT_TRUE(session.GetFieldPages().scent_cells().empty());
    }
}

void CheckScentSpawnChange(float offset) {
    SCOPED_TRACE(offset);
    namespace C = Luminumbra::Components;
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, reopened;
    FieldPageState fields;
    const Luminumbra::world::ScentCell exact{0x3ff0000000000001ULL, 1, 0, 0};
    for (const Luminumbra::world::ScentCellKey key :
         {Luminumbra::world::ScentCellKey{-1023, -511}, {1, 1}, {25, 1}, {1025, 513}, {4000, 1}}) {
        ASSERT_TRUE(fields.set_scent(key, exact));
    }
    std::map<RegionKey, RegionState> owners{{kFieldOwner, RegionState::Active}};
    for (const auto& [key, values] : fields.scent_cells()) {
        owners.try_emplace(FieldPageState::region_of_scent_cell(key), RegionState::Frozen);
    }
    const auto ledger = ScheduledFieldLedger(31, owners);
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    ASSERT_NO_FATAL_FAILURE(CheckSpawnScentConsumers(session));
    const auto emitter = AddSessionScentParticipant(session, 0, 1);
    TickOnce(session);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    session.GetRegistry().destroy(emitter);
    const auto ant_entity = AddSessionScentParticipant(session, 2, 25);
    const auto food_entity = session.GetRegistry().create();
    auto& food = session.GetRegistry().emplace<C::FoodSourceComponent>(food_entity);
    food.cell_x = session.ScentWorldToCellX(25);
    food.cell_z = session.ScentWorldToCellZ(1);
    const auto before = session.GetFieldPages().serialize();
    const auto original_spawn = session.GetMetadata().spawnPoint;
    auto moved_spawn = original_spawn;
    moved_spawn.x += offset;
    moved_spawn.z += offset * 0.5f + 0.75f;
    session.SetSpawnPoint(moved_spawn);
    EXPECT_EQ(session.GetFieldPages().serialize(), before);
    ASSERT_NO_FATAL_FAILURE(CheckSpawnScentConsumers(session));
    ASSERT_NO_FATAL_FAILURE(CheckSessionScentProjection(session));
    const auto& ant = session.GetRegistry().get<C::ForagerComponent>(ant_entity);
    EXPECT_EQ(session.ScentCellToWorldX(ant.cell_x), 25);
    EXPECT_EQ(session.ScentCellToWorldZ(ant.cell_z), 1);
    EXPECT_EQ(session.ScentCellToWorldX(ant.home_x), 29);
    EXPECT_EQ(session.ScentCellToWorldZ(ant.home_z), 1);
    EXPECT_EQ(session.ScentCellToWorldX(food.cell_x), 25);
    EXPECT_EQ(session.ScentCellToWorldZ(food.cell_z), 1);
    // Save immediately after recentering, before any tick at the new spawn.
    ASSERT_TRUE(session.SaveWorld());
    EnableSnapshotSession(reopened, root, jobs);
    ASSERT_TRUE(reopened.LoadWorld(session.GetMetadata().worldId)) << reopened.GetWorldOpenError();
    ASSERT_NO_FATAL_FAILURE(CheckSpawnScentConsumers(reopened));
    ASSERT_NO_FATAL_FAILURE(CheckSessionScentProjection(reopened));
    EXPECT_EQ(SessionScentBits(reopened), SessionScentBits(session));
    EXPECT_EQ(reopened.GetFieldPages().serialize(), before);
    // An inactive tick must not erase historical cells outside the moved window.
    session.GetRegistry().destroy(ant_entity);
    session.GetRegistry().destroy(food_entity);
    TickOnce(session);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_EQ(session.GetFieldPages().serialize(), before);
    session.SetSpawnPoint(original_spawn);
    ASSERT_NO_FATAL_FAILURE(CheckSessionScentProjection(session));
    EXPECT_EQ(session.GetFieldPages().serialize(), before);
    // Repeated and sub-cell moves reuse the same world cells without duplication.
    session.SetSpawnPoint(original_spawn);
    session.SetSpawnPoint(original_spawn + Luminumbra::Vec3(0.25f, 0, 0.25f));
    ASSERT_NO_FATAL_FAILURE(CheckSessionScentProjection(session));
    EXPECT_EQ(session.GetFieldPages().serialize(), before);
    auto invalid_spawn = original_spawn;
    invalid_spawn.x = std::numeric_limits<float>::quiet_NaN();
    session.SetSpawnPoint(invalid_spawn);
    EXPECT_FALSE(session.GetScentField()->SimulationCellAllowed(64, 64));
    EXPECT_EQ(session.GetFieldPages().serialize(), before);
    session.SetSpawnPoint(original_spawn);
    ASSERT_NO_FATAL_FAILURE(CheckSessionScentProjection(session));
    EXPECT_EQ(session.GetFieldPages().serialize(), before);
}

void CheckSessionScentDueQueryCost() {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    std::size_t reference_work = 0;
    for (const std::size_t unrelated : {std::size_t{0}, std::size_t{32}, std::size_t{512}}) {
        SCOPED_TRACE(unrelated);
        GameSession session;
        std::map<RegionKey, RegionState> owners{{kFieldOwner, RegionState::Active}};
        for (std::size_t i = 0; i < unrelated; ++i) {
            owners.emplace(RegionKey{static_cast<std::int32_t>(100 + i), 100}, RegionState::Active);
        }
        const auto ledger = ScheduledFieldLedger(31, owners);
        ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, {}));
        const auto disabled = AddSessionScentParticipant(session, 0, 1);
        session.GetRegistry()
            .get<Luminumbra::Components::SensableComponent>(disabled)
            .scent_channel = -1;
        const auto idle_visits = session.GetScentEligibilityVisits();
        TickOnce(session);
        EXPECT_EQ(session.GetScentEligibilityVisits(), idle_visits);
        ASSERT_FALSE(session.GetRegionSchedulerFault());
        EXPECT_EQ(session.GetRegionSchedule().due.size(), unrelated + 1);
        const auto before = session.GetScentEligibilityVisits();
        EXPECT_TRUE(session.GetScentField()->SimulationCellAllowed(session.ScentWorldToCellX(1),
                                                                   session.ScentWorldToCellZ(1)));
        const auto refreshed = session.GetScentEligibilityVisits();
        EXPECT_LE(refreshed - before, 18u);
        EXPECT_TRUE(session.GetScentField()->SimulationCellAllowed(session.ScentWorldToCellX(1),
                                                                   session.ScentWorldToCellZ(1)));
        const auto warm_query = session.GetScentEligibilityVisits() - refreshed;
        EXPECT_LE(warm_query, 4u);
        EXPECT_GT(refreshed - before, warm_query);
        for (int query = 2; query < 1024; ++query) {
            EXPECT_TRUE(session.GetScentField()->SimulationCellAllowed(
                session.ScentWorldToCellX(1), session.ScentWorldToCellZ(1)));
        }
        const auto work = session.GetScentEligibilityVisits() - before;
        EXPECT_GT(work, 0u);
        EXPECT_LE(work, 14u + 4u * 1024u);
        if (unrelated == 0) {
            reference_work = work;
        } else {
            EXPECT_EQ(work, reference_work);
        }
        AddSessionScentParticipant(session, 0, 1);
        for (int tick = 0; tick < 3; ++tick) {
            TickOnce(session);
            ASSERT_FALSE(session.GetRegionSchedulerFault());
            EXPECT_TRUE(session.GetFieldPages().weather_clocks().empty());
            EXPECT_EQ(session.GetRegionalWeatherScheduleVisits(), 0u);
            EXPECT_EQ(session.GetRegionalWeatherCandidateRecordCount(), 0u);
            EXPECT_EQ(session.GetRegionalWeatherScanCount(), 0u);
        }
    }
}

void CheckActiveScentContinuation(bool moved_spawn) {
    SCOPED_TRACE(moved_spawn);
    namespace C = Luminumbra::Components;
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, reopened;
    const auto ledger = ScheduledFieldLedger(
        31, {{kFieldOwner, RegionState::Active}, {{2, 0}, RegionState::Active}});
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, {}));
    const auto old_emitter = AddSessionScentParticipant(session, 0, 1);
    TickOnce(session);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    session.GetRegistry().destroy(old_emitter);
    const auto historical = session.GetFieldPages().region_bytes(kFieldOwner);
    if (moved_spawn) {
        auto spawn = session.GetMetadata().spawnPoint;
        spawn.x += 1024.25f;
        session.SetSpawnPoint(spawn);
    }
    const auto spawn = session.GetMetadata().spawnPoint;
    const auto emitter = AddSessionScentParticipant(session, 0, spawn.x + 1);
    session.GetRegistry().get<C::TransformComponent>(emitter).position.z = spawn.z;
    TickOnce(session);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    ASSERT_TRUE(session.SaveWorld());
    EnableSnapshotSession(reopened, root, jobs);
    ASSERT_TRUE(reopened.LoadWorld(session.GetMetadata().worldId)) << reopened.GetWorldOpenError();
    reopened.SetReplicatedSimulationAnchors({});
    ASSERT_NO_FATAL_FAILURE(CheckSpawnScentConsumers(reopened));
    ASSERT_NO_FATAL_FAILURE(CheckSessionScentProjection(reopened));
    EXPECT_EQ(SessionScentBits(reopened), SessionScentBits(session));
    const auto restored_emitter = AddSessionScentParticipant(reopened, 0, spawn.x + 1);
    reopened.GetRegistry().get<C::TransformComponent>(restored_emitter).position.z = spawn.z;
    for (int tick = 0; tick < 5; ++tick) {
        const auto previous = SessionScentBits(session);
        TickOnce(session);
        TickOnce(reopened);
        ASSERT_FALSE(session.GetRegionSchedulerFault());
        ASSERT_FALSE(reopened.GetRegionSchedulerFault());
        EXPECT_NE(SessionScentBits(session), previous);
        EXPECT_EQ(SessionScentBits(reopened), SessionScentBits(session));
        EXPECT_EQ(reopened.ComputeScentSubHash(), session.ComputeScentSubHash());
        EXPECT_EQ(reopened.GetFieldPages().serialize(), session.GetFieldPages().serialize());
        EXPECT_EQ(reopened.GetActiveRegionLedger().completed_work(),
                  session.GetActiveRegionLedger().completed_work());
        EXPECT_FALSE(session.GetActiveRegionLedger().completed_work().empty());
        if (moved_spawn) {
            EXPECT_EQ(session.GetFieldPages().region_bytes(kFieldOwner), historical);
        }
        ASSERT_TRUE(session.SaveWorld());
        const auto saved_fields =
            ReadClockTestFile(P::WorldSaveService::field_pages_path(session.GetWorldSaveDir()));
        const auto saved_ledger =
            ReadClockTestFile(P::WorldSaveService::active_regions_path(session.GetWorldSaveDir()));
        ASSERT_TRUE(reopened.SaveWorld());
        EXPECT_EQ(
            ReadClockTestFile(P::WorldSaveService::field_pages_path(reopened.GetWorldSaveDir())),
            saved_fields);
        EXPECT_EQ(
            ReadClockTestFile(P::WorldSaveService::active_regions_path(reopened.GetWorldSaveDir())),
            saved_ledger);
        EXPECT_EQ(reopened.GetActiveRegionLedger().canonical_bytes(),
                  session.GetActiveRegionLedger().canonical_bytes());
    }
}

void CheckHistoricalSessionScent() {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, reopened;
    const auto ledger = ScheduledFieldLedger(31, {{kFieldOwner, RegionState::Active}});
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, {}));
    const auto emitter = AddSessionScentParticipant(session, 0, 1);
    TickOnce(session);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    ASSERT_FALSE(session.GetFieldPages().scent_cells().empty());
    EXPECT_EQ(session.GetActiveRegionLedger().completed_work().size(), 1u);
    EXPECT_GT(session.GetActiveRegionLedger().completed_work().at(kFieldOwner), 0u);
    const auto live = SessionScentBits(session);
    const auto scent = session.GetFieldPages().scent_cells();
    const auto hash = session.ComputeScentSubHash();
    ASSERT_FALSE(hash.empty());
    session.GetRegistry().destroy(emitter);
    TickOnce(session);
    EXPECT_EQ(session.GetFieldPages().scent_cells(), scent);
    EXPECT_EQ(SessionScentBits(session), live);
    EXPECT_EQ(session.ComputeScentSubHash(), hash);
    ASSERT_TRUE(session.SaveWorld());
    const auto save = session.GetWorldSaveDir();
    const auto bytes = ReadClockTestFile(P::WorldSaveService::field_pages_path(save));
    EXPECT_EQ(bytes, session.GetFieldPages().serialize());
    EnableSnapshotSession(reopened, root, jobs);
    ASSERT_TRUE(reopened.LoadWorld(session.GetMetadata().worldId)) << reopened.GetWorldOpenError();
    EXPECT_EQ(SessionScentBits(reopened), live);
    EXPECT_EQ(reopened.GetFieldPages().scent_cells(), scent);
    EXPECT_EQ(reopened.ComputeScentSubHash(), hash);
    ASSERT_TRUE(reopened.SaveWorld());
    EXPECT_EQ(ReadClockTestFile(P::WorldSaveService::field_pages_path(save)), bytes);
    TickOnce(reopened);
    EXPECT_EQ(SessionScentBits(reopened), live);
    EXPECT_EQ(reopened.GetFieldPages().scent_cells(), scent);
}

void CheckProceduralWeatherWithScent(GameSession& session, GameSession& control) {
    AddSessionScentParticipant(session, 0, 1);
    bool saw_storm = false;
    for (int tick = 0; tick < 340; ++tick) {
        SCOPED_TRACE(session.GetWorldClock().tick());
        TickOnce(session);
        TickOnce(control);
        ASSERT_FALSE(session.GetRegionSchedulerFault());
        ASSERT_FALSE(control.GetRegionSchedulerFault());
        ASSERT_EQ(WeatherOverlayHashes(session), WeatherOverlayHashes(control));
        saw_storm = saw_storm || !control.GetWeatherSystem()->StormCells().empty();
        EXPECT_EQ(session.GetRegionalWeatherScheduleVisits(), 0u);
    }
    EXPECT_TRUE(saw_storm);
    EXPECT_TRUE(session.GetFieldPages().has_scent());
    EXPECT_FALSE(session.GetFieldPages().has_ambient());
}

void CheckScentOnlyWeatherContinuation(bool stored_scent) {
    SCOPED_TRACE(stored_scent);
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, control, reopened, control_reopened;
    const auto ledger = ScheduledFieldLedger(31, {{kFieldOwner, RegionState::Active}});
    FieldPageState fields;
    if (stored_scent) {
        ASSERT_TRUE(fields.set_scent({1, 1}, {std::bit_cast<std::uint64_t>(1.0), 0, 0, 0}));
    }
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(control, root, jobs, ledger, {}));
    ASSERT_EQ(WeatherOverlayHashes(session), WeatherOverlayHashes(control));
    ASSERT_NO_FATAL_FAILURE(CheckProceduralWeatherWithScent(session, control));
    const auto scent = session.GetFieldPages().serialize();
    ASSERT_TRUE(session.SaveWorld());
    ASSERT_TRUE(control.SaveWorld());
    EnableSnapshotSession(reopened, root, jobs);
    EnableSnapshotSession(control_reopened, root, jobs);
    ASSERT_TRUE(reopened.LoadWorld(session.GetMetadata().worldId)) << reopened.GetWorldOpenError();
    ASSERT_TRUE(control_reopened.LoadWorld(control.GetMetadata().worldId))
        << control_reopened.GetWorldOpenError();
    reopened.SetReplicatedSimulationAnchors({});
    control_reopened.SetReplicatedSimulationAnchors({});
    EXPECT_EQ(reopened.GetFieldPages().serialize(), scent);
    ASSERT_EQ(WeatherOverlayHashes(reopened), WeatherOverlayHashes(session));
    ASSERT_EQ(WeatherOverlayHashes(reopened), WeatherOverlayHashes(control_reopened));
    ASSERT_NO_FATAL_FAILURE(CheckProceduralWeatherWithScent(reopened, control_reopened));
}

void CheckSessionScentOwnership(int kind, bool frozen_participant) {
    SCOPED_TRACE(kind);
    SCOPED_TRACE(frozen_participant);
    namespace C = Luminumbra::Components;
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    const RegionKey frozen{-1, 0};
    const auto ledger = ScheduledFieldLedger(31,
                                             {{kFieldOwner, RegionState::Active},
                                              {frozen, RegionState::Frozen},
                                              {{4, 0}, RegionState::Frozen}});
    FieldPageState fields;
    const Luminumbra::world::ScentCell exact{
        0x3ff0000000000001ULL, 0x0000000000000001ULL, 0x400921fb54442d18ULL, 0};
    ASSERT_TRUE(fields.set_scent({-2, 1}, exact));
    ASSERT_TRUE(fields.set_scent({2, 1}, exact));
    ASSERT_TRUE(fields.set_scent({2048, 1}, exact));
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    const auto x = session.ScentWorldToCellX(-2);
    const auto z = session.ScentWorldToCellZ(1);
    EXPECT_EQ(std::bit_cast<std::uint64_t>(session.GetScentField()->Sample(0, x, z)), exact[0]);
    EXPECT_EQ(std::bit_cast<std::uint64_t>(session.GetScentField()->Sample(1, x, z)), exact[1]);
    EXPECT_EQ(std::bit_cast<std::uint64_t>(session.GetScentField()->Sample(2, x, z)), exact[2]);
    const auto entity =
        AddSessionScentParticipant(session, kind, frozen_participant ? -1.0f : 1.0f);
    const auto frozen_bytes = fields.region_bytes(frozen);
    const auto active_scent = fields.scent_cells();
    TickOnce(session);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_EQ(session.GetFieldPages().region_bytes(frozen), frozen_bytes);
    EXPECT_EQ(session.GetFieldPages().scent_cells().at({-2, 1}), exact);
    EXPECT_EQ(session.GetFieldPages().scent_cells().at({2048, 1}), exact);
    EXPECT_EQ(std::bit_cast<std::uint64_t>(session.GetScentField()->Sample(0, x, z)), exact[0]);
    EXPECT_EQ(session.GetScentField()->SimulationCellAllowed(
                  session.ScentWorldToCellX(frozen_participant ? -1.0f : 1.0f), z),
              !frozen_participant);
    EXPECT_EQ(session.GetFieldPages().scent_cells() == active_scent, frozen_participant);
    if (kind == 1) {
        const auto wish = session.GetRegistry().get<C::LocomotionIntentComponent>(entity).wish_xz;
        EXPECT_EQ(wish == Luminumbra::Vec2(0), frozen_participant);
    }
    if (kind == 2) {
        const auto& ant = session.GetRegistry().get<C::ForagerComponent>(entity);
        EXPECT_EQ(ant.cell_x == session.ScentWorldToCellX(frozen_participant ? -1.0f : 1.0f),
                  frozen_participant);
    }
    ASSERT_TRUE(session.SaveWorld());
    const auto live = SessionScentBits(session);
    const auto bytes = session.GetFieldPages().serialize();
    ASSERT_TRUE(session.LoadWorldStateFrom(session.GetWorldSaveDir()));
    EXPECT_EQ(SessionScentBits(session), live);
    EXPECT_EQ(session.GetFieldPages().serialize(), bytes);
}

void CheckSessionScentCandidate(bool ambient_pages, bool weather, bool fail_consumer) {
    SCOPED_TRACE(ambient_pages);
    SCOPED_TRACE(weather);
    SCOPED_TRACE(fail_consumer);
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    const RegionKey frozen{4, 0};
    const auto ledger = ScheduledFieldLedger(
        31, {{kFieldOwner, RegionState::Active}, {frozen, RegionState::Frozen}});
    FieldPageState fields;
    if (ambient_pages) {
        ASSERT_TRUE(fields.activate_region(kFieldOwner, 1337, 31));
    }
    if (weather) {
        ASSERT_TRUE(fields.set_weather_age(kFieldOwner, 7));
    }
    ASSERT_TRUE(fields.set_weather_age(frozen, 11));
    ASSERT_TRUE(fields.replace_weather({SessionStorm(frozen, 2100, 108)}, {}));
    ASSERT_TRUE(fields.set_scent({2048, 1}, {std::bit_cast<std::uint64_t>(1.0), 0, 0, 0}));
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    AddSessionScentParticipant(session, 0, 1);
    const auto before = session.GetFieldPages().serialize();
    const auto live_before = SessionScentBits(session);
    if (fail_consumer) {
        session.GetSimulationEventBus().subscribe(
            [](const auto&) { throw std::runtime_error("injected scent candidate fault"); });
        session.GetSimulationEventBus().publish(32, "scent-test", "");
    }
    TickOnce(session);
    if (fail_consumer) {
        ASSERT_TRUE(session.GetRegionSchedulerFault());
        EXPECT_EQ(session.GetFieldPages().serialize(), before);
        EXPECT_EQ(SessionScentBits(session), live_before);
        EXPECT_TRUE(session.GetActiveRegionLedger().completed_work().empty());
        return;
    }
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_GT(session.GetFieldPages().scent_cells().size(), fields.scent_cells().size());
    EXPECT_EQ(session.GetFieldPages().region_bytes(frozen), fields.region_bytes(frozen));
    EXPECT_EQ(session.GetFieldPages().weather_clocks().contains(kFieldOwner),
              ambient_pages || weather);
    if (weather) {
        EXPECT_EQ(session.GetFieldPages().weather_clocks().at(kFieldOwner), 8u);
    }
    if (ambient_pages) {
        EXPECT_NE(session.GetFieldPages().pages(), fields.pages());
    }
    EXPECT_GT(session.GetActiveRegionLedger().completed_work().at(kFieldOwner), 0u);
    const auto live = SessionScentBits(session);
    const auto bytes = session.GetFieldPages().serialize();
    ASSERT_TRUE(session.SaveWorld());
    ASSERT_TRUE(session.LoadWorldStateFrom(session.GetWorldSaveDir()));
    EXPECT_EQ(SessionScentBits(session), live);
    EXPECT_EQ(session.GetFieldPages().serialize(), bytes);
}

void CheckReducedSessionScentCadence() {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    const std::uint64_t due_tick = std::uint64_t{32} + ActiveRegionLedger::phase(kFieldOwner, 2);
    const auto ledger = ScheduledFieldLedger(due_tick - 1, {{kFieldOwner, RegionState::Reduced}});
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, {}));
    AddSessionScentParticipant(session, 0, 1);
    TickOnce(session);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    ASSERT_FALSE(session.GetFieldPages().scent_cells().empty());
    EXPECT_EQ(session.GetRegionSchedule().due, std::vector<RegionKey>{kFieldOwner});
    const auto live = SessionScentBits(session);
    const auto fields = session.GetFieldPages().serialize();
    TickOnce(session);
    EXPECT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_TRUE(session.GetRegionSchedule().due.empty());
    EXPECT_EQ(SessionScentBits(session), live);
    EXPECT_EQ(session.GetFieldPages().serialize(), fields);
}

void CheckSessionScentCrossOwnerCadence(bool due_participant) {
    SCOPED_TRACE(due_participant);
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    const RegionKey reduced{-1, 0};
    // The persisted cadence phase for this owner is 1, so tick 32 must leave it untouched.
    ASSERT_EQ(ActiveRegionLedger::phase(reduced, 2), 1u);
    const auto ledger = ScheduledFieldLedger(
        31, {{kFieldOwner, RegionState::Active}, {reduced, RegionState::Reduced}});
    FieldPageState fields;
    const Luminumbra::world::ScentCell exact{std::bit_cast<std::uint64_t>(8.0), 0, 0, 0};
    ASSERT_TRUE(fields.set_scent({-1, 1}, exact));
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    // A sensor keeps the solver eligible without depositing scent. Placing it in the
    // reduced owner also exercises HasScentBorderWork when no interior is due.
    AddSessionScentParticipant(session, 1, due_participant ? 1.0f : -1.0f);
    const auto before = SessionScentBits(session);
    const auto reduced_bytes = fields.region_bytes(reduced);
    ASSERT_NO_FATAL_FAILURE(TickOnce(session));
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    ASSERT_EQ(session.GetWorldClock().tick(), 32u);
    ASSERT_EQ(session.GetRegionSchedule().due, std::vector<RegionKey>{kFieldOwner});
    const auto x = session.ScentWorldToCellX(0), z = session.ScentWorldToCellZ(1);
    EXPECT_DOUBLE_EQ(session.GetScentField()->Sample(0, x, z),
                     due_participant ? FieldPageState::kScentFloor : 0.0);
    EXPECT_DOUBLE_EQ(session.GetScentField()->Sample(0, x - 1, z), 8.0);
    EXPECT_EQ(session.GetFieldPages().region_bytes(reduced), reduced_bytes);
    ASSERT_NO_FATAL_FAILURE(CheckSessionScentProjection(session));
    if (!due_participant) {
        EXPECT_EQ(SessionScentBits(session), before);
    }
    // Tick 33 satisfies both the active cadence and the reduced owner's cadence phase.
    ASSERT_NO_FATAL_FAILURE(TickOnce(session));
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    const std::vector<RegionKey> both_due{reduced, kFieldOwner};
    ASSERT_EQ(session.GetRegionSchedule().due, both_due);
    EXPECT_GT(session.GetScentField()->Sample(0, x, z), FieldPageState::kScentFloor);
    EXPECT_NE(session.GetFieldPages().region_bytes(reduced), reduced_bytes);
    ASSERT_NO_FATAL_FAILURE(CheckSessionScentProjection(session));
}

void CheckFloorOnlyScentSaveOwnership() {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    const auto ledger = ScheduledFieldLedger(31, {{kFieldOwner, RegionState::Active}});
    const auto floor = std::bit_cast<std::uint64_t>(FieldPageState::kScentFloor);
    const std::array<Luminumbra::world::ScentCell, 1> cells{
        Luminumbra::world::ScentCell{floor, floor, floor, floor}};
    FieldPageState fields;
    std::map<RegionKey, std::uint64_t> work;
    ASSERT_TRUE(fields.capture_scent_window({1, 1}, 1, cells, work));
    ASSERT_TRUE(fields.scent_cells().empty());
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    ASSERT_NO_FATAL_FAILURE(CheckSessionScentProjection(session));
    EXPECT_FALSE(session.GetScentField()->SimulationCellAllowed(session.ScentWorldToCellX(1),
                                                                session.ScentWorldToCellZ(1)));
    EXPECT_FALSE(session.ComputeScentSubHash().empty());
    ASSERT_TRUE(session.SaveWorld());
    ASSERT_TRUE(session.LoadWorldStateFrom(session.GetWorldSaveDir()));
    EXPECT_EQ(session.GetFieldPages().serialize(), fields.serialize());
    // A compact-only owner must pass the same orphan check as an explicit scent cell.
    FieldPageState orphan;
    ASSERT_TRUE(orphan.capture_scent_window({2048, 1}, 1, cells, work));
    const auto input = root.path() / "scheduled-field-input";
    std::vector<std::string> errors;
    P::WorldSaveService::SaveFactsScope facts(input);
    P::SaveTransaction transaction(input, ledger.tick(), {}, &P::WorldSaveService::member_digest);
    ASSERT_TRUE(transaction.begin(&errors));
    ASSERT_TRUE(P::WorldSaveService::save_field_pages(orphan, input, &errors));
    EXPECT_FALSE(P::WorldSaveService::validate_field_pages(input, ledger, &errors));
}

void CheckDisjointSessionScentWindows() {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    std::map<RegionKey, RegionState> owners;
    for (std::int32_t window = 0; window < 6; ++window) {
        owners.emplace(RegionKey{window * 4, 0}, RegionState::Active);
    }
    const auto ledger = ScheduledFieldLedger(31, owners);
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, {}));
    for (int window = 0; window < 6; ++window) {
        session.SetSpawnPoint({static_cast<float>(window * 2048 + 128), 0, 128});
        const auto emitter =
            AddSessionScentParticipant(session, 0, session.GetMetadata().spawnPoint.x + 1);
        session.GetRegistry().get<Luminumbra::Components::TransformComponent>(emitter).position.z =
            129;
        for (int tick = 0; tick < 2; ++tick) {
            TickOnce(session);
            ASSERT_FALSE(session.GetRegionSchedulerFault()) << "window=" << window;
            EXPECT_GT(session.GetScentField()->Sample(0, 65, 65), 1.0e-9);
            EXPECT_EQ(session.GetRegionalWeatherScheduleVisits(), 0u);
        }
        session.GetRegistry().destroy(emitter);
    }
    EXPECT_LE(session.GetFieldPages().scent_cells().size(), FieldPageState::kMaxScentCells);
    EXPECT_LE(session.GetFieldPages().scent_floor_blocks(), FieldPageState::kMaxScentFloorBlocks);
    ASSERT_NO_FATAL_FAILURE(CheckSessionScentProjection(session));
    const auto bytes = session.GetFieldPages().serialize();
    const auto bits = SessionScentBits(session);
    ASSERT_TRUE(session.SaveWorld());
    ASSERT_TRUE(session.LoadWorldStateFrom(session.GetWorldSaveDir()));
    EXPECT_EQ(session.GetFieldPages().serialize(), bytes);
    EXPECT_EQ(SessionScentBits(session), bits);
    TickOnce(session);
    EXPECT_FALSE(session.GetRegionSchedulerFault());
}

void CheckSessionScentFaultRollback(bool move_spawn) {
    SCOPED_TRACE(move_spawn);
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    const auto ledger = ScheduledFieldLedger(31, {{kFieldOwner, RegionState::Active}});
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, {}));
    AddSessionScentParticipant(session, 0, 1);
    const auto live = SessionScentBits(session);
    session.GetSimulationEventBus().subscribe([&](const auto&) {
        if (move_spawn) {
            session.SetSpawnPoint(session.GetMetadata().spawnPoint + Luminumbra::Vec3(24, 0, 8));
        }
        throw std::runtime_error("injected scent consumer fault");
    });
    session.GetSimulationEventBus().publish(32, "scent-test", "");
    TickOnce(session);
    const auto fault = session.GetRegionSchedulerFault();
    ASSERT_TRUE(fault.has_value());
    if (fault.has_value()) {
        EXPECT_EQ(fault->message, "injected scent consumer fault");
    }
    EXPECT_TRUE(session.GetFieldPages().empty());
    EXPECT_EQ(SessionScentBits(session), live);
    EXPECT_TRUE(session.GetActiveRegionLedger().completed_work().empty());
}

void CheckScentCallbackSpawnMove(float offset) {
    SCOPED_TRACE(offset);
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, reopened;
    const auto ledger = ScheduledFieldLedger(31, {{kFieldOwner, RegionState::Active}});
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, {}));
    AddSessionScentParticipant(session, 0, 1);
    TickOnce(session);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    const auto before = session.GetFieldPages().serialize();
    auto spawn = session.GetMetadata().spawnPoint;
    spawn.x += offset;
    spawn.z += 8.75f;
    int callbacks = 0;
    session.GetSimulationEventBus().subscribe([&](const auto&) {
        ++callbacks;
        session.SetSpawnPoint(spawn);
        session.SetSpawnPoint(spawn); // Repeated moves must retain the pending refresh.
    });
    session.GetSimulationEventBus().publish(33, "scent-test", "");
    TickOnce(session);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    ASSERT_EQ(callbacks, 1);
    EXPECT_NE(session.GetFieldPages().serialize(), before);
    // The shifted live grid must include the candidate published after the callback.
    ASSERT_NO_FATAL_FAILURE(CheckSessionScentProjection(session));
    const auto live = SessionScentBits(session);
    ASSERT_TRUE(session.SaveWorld());
    EnableSnapshotSession(reopened, root, jobs);
    ASSERT_TRUE(reopened.LoadWorld(session.GetMetadata().worldId)) << reopened.GetWorldOpenError();
    reopened.SetReplicatedSimulationAnchors({});
    EXPECT_EQ(SessionScentBits(reopened), live);
    EXPECT_EQ(reopened.GetFieldPages().serialize(), session.GetFieldPages().serialize());
    AddSessionScentParticipant(reopened, 0, 1);
    TickOnce(session);
    TickOnce(reopened);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    ASSERT_FALSE(reopened.GetRegionSchedulerFault());
    EXPECT_EQ(SessionScentBits(reopened), SessionScentBits(session));
    EXPECT_EQ(reopened.GetFieldPages().serialize(), session.GetFieldPages().serialize());
    EXPECT_EQ(reopened.GetActiveRegionLedger().completed_work(),
              session.GetActiveRegionLedger().completed_work());
}

void CheckSessionScentCapacityRetention(bool floors, RegionState historical_state) {
    SCOPED_TRACE(floors);
    SCOPED_TRACE(static_cast<int>(historical_state));
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    FieldPageState fields;
    const auto bits = std::bit_cast<std::uint64_t>(floors ? FieldPageState::kScentFloor : 1.0);
    const std::vector<Luminumbra::world::ScentCell> cells(
        128u * 128u, Luminumbra::world::ScentCell{bits, bits, bits, bits});
    std::map<RegionKey, std::uint64_t> work;
    const int extent = floors ? 512 : 256;
    for (int x = 0; x < extent; x += 128) {
        for (int z = 0; z < extent; z += 128) {
            ASSERT_TRUE(fields.capture_scent_window({1024 + x, z}, 128, cells, work));
        }
    }
    ASSERT_EQ(floors ? fields.scent_floor_blocks() : fields.scent_cells().size(),
              floors ? FieldPageState::kMaxScentFloorBlocks : FieldPageState::kMaxScentCells);
    const RegionKey frozen_owner{2, 0};
    auto ledger = ScheduledFieldLedger(
        31, {{kFieldOwner, RegionState::Active}, {frozen_owner, RegionState::Frozen}});
    if (historical_state != RegionState::Frozen) {
        // Typed thaw fixture: the ledger retains the previous freeze's digest and timestamp.
        auto& record =
            const_cast<std::map<RegionKey, ActiveRegionRecord>&>(ledger.records()).at(frozen_owner);
        record.state = historical_state;
        record.cadence_shift =
            historical_state == RegionState::Active ? std::uint8_t{0} : std::uint8_t{2};
        record.cadence_phase = ActiveRegionLedger::phase(frozen_owner, record.cadence_shift);
    }
    GameSession session;
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, fields));
    session.SetSpawnPoint({128, 0, 128});
    const auto emitter = AddSessionScentParticipant(session, 0, 129);
    session.GetRegistry().get<Luminumbra::Components::TransformComponent>(emitter).position.z = 129;
    const auto before = fields.serialize();
    const auto frozen_digest = ledger.records().at(frozen_owner).frozen_digest;
    for (int tick = 0; tick < 3; ++tick) {
        TickOnce(session);
        ASSERT_FALSE(session.GetRegionSchedulerFault());
        EXPECT_EQ(session.GetWorldClock().tick(), 32u + static_cast<unsigned>(tick));
        if (historical_state == RegionState::Frozen) {
            EXPECT_EQ(session.GetFieldPages().serialize(), before);
            EXPECT_FALSE(session.GetActiveRegionLedger().completed_work().contains(frozen_owner));
        } else {
            EXPECT_NE(session.GetFieldPages().region_bytes(frozen_owner),
                      fields.region_bytes(frozen_owner));
            EXPECT_GT(session.GetScentField()->Sample(0, 65, 65), FieldPageState::kScentFloor);
            EXPECT_GT(session.GetFieldPages().scent_at({129, 129})[0],
                      std::bit_cast<std::uint64_t>(FieldPageState::kScentFloor));
            if (tick == 0) {
                const auto& completed = session.GetActiveRegionLedger().completed_work();
                ASSERT_TRUE(completed.contains(frozen_owner));
                EXPECT_GT(completed.at(frozen_owner), 0u);
            }
        }
        EXPECT_EQ(session.GetActiveRegionLedger().records().at(frozen_owner).state,
                  historical_state);
        EXPECT_EQ(session.GetActiveRegionLedger().records().at(frozen_owner).frozen_digest,
                  frozen_digest);
        ASSERT_NO_FATAL_FAILURE(CheckSessionScentProjection(session));
    }
    const auto saved = session.GetFieldPages().serialize();
    ASSERT_TRUE(session.SaveWorld());
    ASSERT_TRUE(session.LoadWorldStateFrom(session.GetWorldSaveDir()));
    EXPECT_EQ(session.GetFieldPages().serialize(), saved);
    EXPECT_EQ(session.GetActiveRegionLedger().records().at(frozen_owner).frozen_digest,
              frozen_digest);
    ASSERT_NO_FATAL_FAILURE(CheckSessionScentProjection(session));
    TickOnce(session);
    EXPECT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_EQ(session.GetWorldClock().tick(), 35u);
}

void CheckSessionScentUngatedArithmetic(bool active_regions) {
    SCOPED_TRACE(active_regions);
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    if (active_regions) {
        const auto ledger = ScheduledFieldLedger(31,
                                                 {{{-1, -1}, RegionState::Active},
                                                  {{-1, 0}, RegionState::Active},
                                                  {{0, -1}, RegionState::Active},
                                                  {{0, 0}, RegionState::Active}});
        ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, {}));
    } else {
        EnableSnapshotSession(session, root, jobs);
        session.SetActiveRegionsEnabled(false);
        ASSERT_TRUE(session.CreateWorld("Legacy scent arithmetic", "1337", "default"));
    }
    AddSessionScentParticipant(session, 0, 1);
    luminumbra::ai::ScentField expected(128, 128, 4);
    expected.Deposit(0, session.ScentWorldToCellX(1), session.ScentWorldToCellZ(1), 8.0);
    const auto wind = session.GetWindFieldSystem()->SampleWind(session.GetMetadata().spawnPoint);
    const double dt = session.GetSimulationClock().fixed_dt();
    expected.Step(
        0.25, 4, 0.05, static_cast<double>(wind.x) * dt, static_cast<double>(wind.y) * dt);
    expected.Clamp(1.0e-9, 1.0e6);
    TickOnce(session);
    EXPECT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_EQ(SessionScentBits(session), ScentBits(expected));
    EXPECT_EQ(session.GetFieldPages().scent_cells().empty(), !active_regions);
}

void CheckEmptyScentCompatibility(bool active_regions) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    EnableSnapshotSession(session, root, jobs);
    session.SetActiveRegionsEnabled(active_regions);
    ASSERT_TRUE(session.CreateWorld("Empty scent", "1337", "default"));
    const auto live = SessionScentBits(session);
    TickOnce(session);
    EXPECT_EQ(SessionScentBits(session), live);
    EXPECT_TRUE(session.ComputeScentSubHash().empty());
    EXPECT_TRUE(session.GetFieldPages().empty());
    ASSERT_TRUE(session.SaveWorld());
    auto disk = SavedPresetTestDiskBytes(session.GetWorldSaveDir());
    ASSERT_TRUE(session.SaveWorld());
    auto resaved = SavedPresetTestDiskBytes(session.GetWorldSaveDir());
    // The snapshot index records the save generation, which advances on every save.
    disk.erase("chunks/region/snapshot.json");
    resaved.erase("chunks/region/snapshot.json");
    EXPECT_EQ(resaved, disk);
    EXPECT_FALSE(fs::exists(P::WorldSaveService::field_pages_path(session.GetWorldSaveDir())));
    ASSERT_TRUE(session.LoadWorldStateFrom(session.GetWorldSaveDir()));
    EXPECT_EQ(SessionScentBits(session), live);
    EXPECT_TRUE(session.ComputeScentSubHash().empty());
}

} // namespace

TEST(GameSessionHeadlessWorldTest, NewWorldScentUsesSpawnAndLegacyMovesKeepGridBits) {
    for (const bool active_regions : {false, true}) {
        ASSERT_NO_FATAL_FAILURE(CheckNewWorldScentSpawnMapping(active_regions));
    }
}

TEST(GameSessionHeadlessWorldTest, RegionalScentRecentersOnSpawnWithoutChangingHistoricalCells) {
    for (const float offset : {32.25f, 1024.25f, -1024.25f}) {
        ASSERT_NO_FATAL_FAILURE(CheckScentSpawnChange(offset));
    }
}

TEST(GameSessionHeadlessWorldTest, ScentEligibilityWorkStaysFixedAsDueRegionsGrow) {
    ASSERT_NO_FATAL_FAILURE(CheckSessionScentDueQueryCost());
}

TEST(GameSessionHeadlessWorldTest, ActiveScentContinuesBitExactlyAcrossReopenAtCurrentSpawn) {
    for (const bool moved_spawn : {false, true}) {
        ASSERT_NO_FATAL_FAILURE(CheckActiveScentContinuation(moved_spawn));
    }
}

TEST(GameSessionHeadlessWorldTest, HistoricalScentSurvivesEmitterRemovalAndReopenByteExactly) {
    ASSERT_NO_FATAL_FAILURE(CheckHistoricalSessionScent());
}

TEST(GameSessionHeadlessWorldTest, ScentOnlyPublicationPreservesProceduralWeatherAcrossReopen) {
    ASSERT_NO_FATAL_FAILURE(CheckScentOnlyWeatherContinuation(false));
}

TEST(GameSessionHeadlessWorldTest, ScentOnlyRestorePreservesProceduralWeatherAcrossReopen) {
    ASSERT_NO_FATAL_FAILURE(CheckScentOnlyWeatherContinuation(true));
}

TEST(GameSessionHeadlessWorldTest, RegionalScentEmittersAndConsumersRespectFrozenOwners) {
    for (const int kind : {0, 1, 2}) {
        ASSERT_NO_FATAL_FAILURE(CheckSessionScentOwnership(kind, false));
        ASSERT_NO_FATAL_FAILURE(CheckSessionScentOwnership(kind, true));
    }
}

TEST(GameSessionHeadlessWorldTest, RegionalScentFaultDiscardsCaptureAndRestoresLiveBytes) {
    for (const bool move_spawn : {false, true}) {
        ASSERT_NO_FATAL_FAILURE(CheckSessionScentFaultRollback(move_spawn));
    }
}

TEST(GameSessionHeadlessWorldTest, EmptyScentPreservesLiveAndSaveBytes) {
    for (const bool active_regions : {false, true}) {
        ASSERT_NO_FATAL_FAILURE(CheckEmptyScentCompatibility(active_regions));
    }
}

TEST(GameSessionHeadlessWorldTest, RegionalAndLegacyScentKeepUngatedSolverArithmetic) {
    for (const bool active_regions : {false, true}) {
        ASSERT_NO_FATAL_FAILURE(CheckSessionScentUngatedArithmetic(active_regions));
    }
}

TEST(GameSessionHeadlessWorldTest, RegionalScentParticipantsWaitForTheirOwnersCadence) {
    ASSERT_NO_FATAL_FAILURE(CheckReducedSessionScentCadence());
}

TEST(GameSessionHeadlessWorldTest, RegionalScentCrossOwnerExchangeRequiresBothOwnersDue) {
    for (const bool due_participant : {true, false}) {
        ASSERT_NO_FATAL_FAILURE(CheckSessionScentCrossOwnerCadence(due_participant));
    }
}

TEST(GameSessionHeadlessWorldTest, RegionalScentPublishesWithAmbientAndWeatherCandidates) {
    for (const bool ambient_pages : {false, true}) {
        for (const bool weather : {false, true}) {
            for (const bool fail_consumer : {false, true}) {
                ASSERT_NO_FATAL_FAILURE(
                    CheckSessionScentCandidate(ambient_pages, weather, fail_consumer));
            }
        }
    }
}

TEST(GameSessionHeadlessWorldTest, RegionalScentContinuesAcrossSixDisjointWindows) {
    ASSERT_NO_FATAL_FAILURE(CheckDisjointSessionScentWindows());
}

TEST(GameSessionHeadlessWorldTest, CompactOnlyScentRetainsSavedOwnerValidationAndHash) {
    ASSERT_NO_FATAL_FAILURE(CheckFloorOnlyScentSaveOwnership());
}

TEST(GameSessionHeadlessWorldTest, CallbackSpawnMovePublishesMatchingLiveAndSavedScent) {
    for (const float offset : {-24.25f, 24.25f}) {
        ASSERT_NO_FATAL_FAILURE(CheckScentCallbackSpawnMove(offset));
    }
}

TEST(GameSessionHeadlessWorldTest, FrozenScentCapacityRefusalKeepsSimulationAdvancing) {
    for (const bool floors : {false, true}) {
        ASSERT_NO_FATAL_FAILURE(
            CheckSessionScentCapacityRetention(floors, Luminumbra::world::RegionState::Frozen));
    }
}

TEST(GameSessionHeadlessWorldTest, ThawedScentHistoryRetiresAtCapacityDespiteHistoricalDigest) {
    for (const auto state :
         {Luminumbra::world::RegionState::Active, Luminumbra::world::RegionState::Reduced}) {
        for (const bool floors : {false, true}) {
            ASSERT_NO_FATAL_FAILURE(CheckSessionScentCapacityRetention(floors, state));
        }
    }
}

namespace {
void CheckRegionalEnergyContinuation() {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, reopened;
    const RegionKey active{0, 0}, empty{2, 0}, frozen{4, 0};
    auto ledger = ScheduledFieldLedger(31,
                                       {{active, RegionState::Active},
                                        {empty, RegionState::Reduced},
                                        {frozen, RegionState::Frozen}});
    ledger.pin(active, false, WorldClock(31));
    ASSERT_TRUE(ledger.set_energy_age(active, 6));
    ASSERT_TRUE(ledger.set_energy_age(empty, 5));
    ASSERT_TRUE(ledger.set_energy_age(frozen, 3));
    FieldPageState fields;
    ASSERT_TRUE(fields.activate_region(active, 1337, 0));
    const std::string initial = "EFS1 2 0\nP 0 0 0:60000\nP 5 0 10:777\n";
    ASSERT_NO_FATAL_FAILURE(
        LoadScheduledFieldFixture(session, root, jobs, ledger, fields, initial));
    TickOnce(session); // Active age 7; the reduced empty owner has offset 3, so is not due.
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_EQ(session.GetEnergyFieldState()->at_cell(0, 0), 60000u);
    EXPECT_EQ(session.GetActiveRegionLedger().energy_ages().at(active), 7u);
    EXPECT_EQ(session.GetActiveRegionLedger().energy_ages().at(empty), 5u);
    const auto values = session.GetEnergyFieldState()->SerializeRegionalRecord();
    const auto ages = session.GetActiveRegionLedger().energy_ages();
    const auto hash = session.ComputeAetherStateSubHash();
    ASSERT_TRUE(session.SaveWorld());
    const auto save = session.GetWorldSaveDir();
    auto disk = SavedPresetTestDiskBytes(save);
    ASSERT_TRUE(session.SaveWorldState());
    ASSERT_TRUE(session.SaveWorldStateTo(root.path() / "energy-copy"));
    auto resaved = SavedPresetTestDiskBytes(save);
    disk.erase("chunks/region/snapshot.json");
    resaved.erase("chunks/region/snapshot.json");
    EXPECT_EQ(resaved, disk);
    EXPECT_EQ(session.GetEnergyFieldState()->SerializeRegionalRecord(), values);
    EXPECT_EQ(session.GetActiveRegionLedger().energy_ages(), ages);
    EXPECT_EQ(session.ComputeAetherStateSubHash(), hash);
    EXPECT_EQ(ReadClockTestFile(save / "aether_state.efs"), values);
    EnableSnapshotSession(reopened, root, jobs);
    reopened.SetAetherStateEnabled(true);
    ASSERT_TRUE(reopened.LoadWorld(session.GetMetadata().worldId)) << reopened.GetWorldOpenError();
    reopened.SetReplicatedSimulationAnchors({});
    EXPECT_EQ(reopened.GetFieldPages().serialize(), session.GetFieldPages().serialize());
    EXPECT_EQ(reopened.GetActiveRegionLedger().energy_ages(), ages);
    EXPECT_EQ(reopened.GetEnergyFieldState()->SerializeRegionalRecord(), values);
    for (std::uint64_t tick = 33; tick <= 44; ++tick) {
        SCOPED_TRACE(tick);
        TickOnce(session);
        TickOnce(reopened);
        ASSERT_FALSE(session.GetRegionSchedulerFault());
        ASSERT_FALSE(reopened.GetRegionSchedulerFault());
        EXPECT_EQ(reopened.GetEnergyFieldState()->SerializeRegionalRecord(),
                  session.GetEnergyFieldState()->SerializeRegionalRecord());
        EXPECT_EQ(reopened.GetActiveRegionLedger().canonical_bytes(),
                  session.GetActiveRegionLedger().canonical_bytes());
        EXPECT_EQ(reopened.GetFieldPages().serialize(), session.GetFieldPages().serialize());
        EXPECT_EQ(reopened.ComputeAetherStateSubHash(), session.ComputeAetherStateSubHash());
        if (tick == 33) {
            EXPECT_LT(session.GetEnergyFieldState()->total_raw(), 60777u);
        }
        if (tick == 35) {
            EXPECT_EQ(session.GetRegionSchedule().due, (std::vector<RegionKey>{empty, active}));
        }
    }
    EXPECT_EQ(session.GetActiveRegionLedger().energy_ages().at(active), 19u);
    EXPECT_EQ(session.GetActiveRegionLedger().energy_ages().at(empty), 8u);
    EXPECT_EQ(session.GetActiveRegionLedger().energy_ages().at(frozen), 3u);
    EXPECT_EQ(session.GetEnergyFieldState()->at_cell(85, 0), 777u);
}

void CheckEmptyRegionalEnergyAge() {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, reopened;
    auto ledger = ScheduledFieldLedger(31, {{kFieldOwner, RegionState::Active}});
    ASSERT_TRUE(ledger.set_energy_age(kFieldOwner, 0));
    ASSERT_NO_FATAL_FAILURE(
        LoadScheduledFieldFixture(session, root, jobs, ledger, {}, std::string{}));
    for (int tick = 0; tick < 7; ++tick) {
        TickOnce(session);
    }
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_TRUE(session.GetFieldPages().empty());
    EXPECT_TRUE(session.ComputeAetherStateSubHash().empty());
    ASSERT_TRUE(session.SaveWorld());
    const auto save = session.GetWorldSaveDir();
    EXPECT_FALSE(fs::exists(save / "aether_state.efs"));
    EnableSnapshotSession(reopened, root, jobs);
    reopened.SetAetherStateEnabled(true);
    ASSERT_TRUE(reopened.LoadWorld(session.GetMetadata().worldId)) << reopened.GetWorldOpenError();
    reopened.SetReplicatedSimulationAnchors({});
    for (auto* current : {&session, &reopened}) {
        EXPECT_EQ(current->GetActiveRegionLedger().energy_ages().at(kFieldOwner), 7u);
        ASSERT_TRUE(current->GetEnergyFieldState()->QueueRegionalDeposit(1, 0, 0, 0, 60000));
        TickOnce(*current); // Age 8 fires, although the absolute tick is 39.
        EXPECT_EQ(current->GetActiveRegionLedger().energy_ages().at(kFieldOwner), 8u);
        EXPECT_LT(current->GetEnergyFieldState()->total_raw(), 60000u);
        EXPECT_GT(current->GetEnergyFieldState()->total_raw(), 0u);
    }
    EXPECT_EQ(reopened.ComputeAetherStateSubHash(), session.ComputeAetherStateSubHash());
    const auto values = session.GetEnergyFieldState()->SerializeRegionalRecord();
    const auto ages = session.GetActiveRegionLedger().energy_ages();
    session.GetSimulationEventBus().subscribe(
        [](const auto&) { throw std::runtime_error("injected energy consumer fault"); });
    session.GetSimulationEventBus().publish(40, "energy-test", "");
    ASSERT_TRUE(session.GetEnergyFieldState()->QueueRegionalDeposit(2, 0, 0, 0, 1000));
    TickOnce(session);
    ASSERT_TRUE(session.GetRegionSchedulerFault());
    EXPECT_EQ(session.GetEnergyFieldState()->SerializeRegionalRecord(), values);
    EXPECT_EQ(session.GetActiveRegionLedger().energy_ages(), ages);
    EXPECT_TRUE(session.GetActiveRegionLedger().completed_work().empty());
}

void CheckEnergyCompatibility(bool active_regions, bool energy_enabled) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    EnableSnapshotSession(session, root, jobs);
    session.SetActiveRegionsEnabled(active_regions);
    session.SetAetherStateEnabled(energy_enabled);
    ASSERT_TRUE(session.CreateWorld("Energy compatibility", "1337", "default"));
    TickOnce(session);
    EXPECT_TRUE(session.ComputeAetherStateSubHash().empty());
    ASSERT_TRUE(session.SaveWorld());
    const auto save = session.GetWorldSaveDir();
    auto disk = SavedPresetTestDiskBytes(save);
    const auto hash = SessionWorldHash(session);
    ASSERT_TRUE(session.SaveWorld());
    auto resaved = SavedPresetTestDiskBytes(save);
    disk.erase("chunks/region/snapshot.json");
    resaved.erase("chunks/region/snapshot.json");
    EXPECT_EQ(resaved, disk);
    EXPECT_EQ(SessionWorldHash(session), hash);
    if (!active_regions && energy_enabled) {
        EXPECT_EQ(ReadClockTestTextRecord(save / "aether_state.efs"), "EFS1 2 7\n");
        for (int tick = 1; tick < 8; ++tick) {
            TickOnce(session);
        }
        ASSERT_TRUE(session.SaveWorld());
        EXPECT_FALSE(fs::exists(save / "aether_state.efs"));
    } else {
        EXPECT_FALSE(fs::exists(save / "aether_state.efs"));
    }
}

} // namespace

TEST(GameSessionHeadlessWorldTest, RegionalEnergySavesObserveAndReopenContinuesOwnerCadence) {
    ASSERT_NO_FATAL_FAILURE(CheckRegionalEnergyContinuation());
}

TEST(GameSessionHeadlessWorldTest, EmptyRegionalEnergyRetainsAgeForTheFirstDepositAfterReopen) {
    ASSERT_NO_FATAL_FAILURE(CheckEmptyRegionalEnergyAge());
}

TEST(GameSessionHeadlessWorldTest, DisabledAndEmptyEnergyPreserveLiveAndSavedBytes) {
    for (const bool active_regions : {false, true}) {
        for (const bool energy_enabled : {false, true}) {
            ASSERT_NO_FATAL_FAILURE(CheckEnergyCompatibility(active_regions, energy_enabled));
        }
    }
}

namespace {
using EnergyState = luminumbra::fields::EnergyFieldState;

std::string LegacyEnergyCapacityRecord(std::size_t pages,
                                       int channels,
                                       bool dense,
                                       std::map<RegionKey, RegionState>& owners) {
    std::string cells;
    const auto values = dense ? 256u * static_cast<std::size_t>(channels) : 1u;
    for (std::size_t index = 0; index < values; ++index) {
        cells += " " + std::to_string(index) + (dense ? ":65535" : ":1");
    }
    std::string record = "EFS1 " + std::to_string(channels) + " 8\n";
    record.reserve(record.size() + pages * (cells.size() + 16));
    for (std::size_t index = 0; index < pages; ++index) {
        const int page = index == 0 ? 0 : 1000 + 4 * static_cast<int>(index - 1);
        record += "P ";
        record += std::to_string(page);
        record += " 0";
        record += cells;
        record += '\n';
        // Dense pages can straddle two owners; sparse pages own just their first cell.
        for (const int dx : {0, dense ? 15 : 0}) {
            owners.try_emplace(EnergyState::OwnerOfCell(page * 16 + dx, 0), RegionState::Frozen);
        }
    }
    return record;
}

void CheckLegacyEnergyCapacityAdmission(bool oversized) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    auto live_ledger = ScheduledFieldLedger(31, {{{0, 0}, RegionState::Active}});
    ASSERT_TRUE(live_ledger.set_energy_age({0, 0}, 3));
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(
        session, root, jobs, live_ledger, {}, std::string{"EFS1 2 0\nP 0 0 0:60000\n"}));
    const auto live_values = session.GetEnergyFieldState()->SerializeRegionalRecord();
    const auto live_registers = session.GetActiveRegionLedger().encode();
    const auto live_tick = session.GetWorldClock().tick();
    std::map<RegionKey, RegionState> owners{{{-1, 0}, RegionState::Active}};
    const auto pages = luminumbra::fields::kEnergyReadPages + static_cast<std::size_t>(oversized);
    auto record = LegacyEnergyCapacityRecord(pages, 16, false, owners);
    // Exercise the full decoded legacy reader capacity with a small text fixture.
    // Every value owner is frozen, so it survives even a cadence fire.
    const auto ledger = ScheduledFieldLedger(31, owners);
    const auto input = root.path() / "legacy-energy-capacity";
    fs::create_directories(input);
    nlohmann::json metadata = {{"container_version", P::WorldSaveService::kContainerVersion}};
    WorldClock(31).write_metadata(metadata);
    ASSERT_TRUE(
        P::WorldSaveService::write_file_durably(input / "world_info.json", metadata.dump()));
    ASSERT_TRUE(P::WorldSaveService::write_file_durably(
        P::WorldSaveService::active_regions_path(input), ledger.encode()));
    ASSERT_TRUE(P::WorldSaveService::write_file_durably(input / "aether_state.efs", record));
    if (oversized) {
        EXPECT_FALSE(session.LoadWorldStateFrom(input));
        EXPECT_NE(session.GetWorldOpenError().find("Corrupt regional energy record"),
                  std::string::npos);
        EXPECT_EQ(session.GetEnergyFieldState()->SerializeRegionalRecord(), live_values);
        EXPECT_EQ(session.GetActiveRegionLedger().encode(), live_registers);
        EXPECT_EQ(session.GetWorldClock().tick(), live_tick);
    } else {
        ASSERT_TRUE(session.LoadWorldStateFrom(input)) << session.GetWorldOpenError();
        session.SetReplicatedSimulationAnchors({});
        for (int tick = 0; tick < 8; ++tick) {
            TickOnce(session);
            ASSERT_FALSE(session.GetRegionSchedulerFault());
        }
        EXPECT_EQ(session.GetActiveRegionLedger().energy_ages().at({-1, 0}), 8u);
        record.replace(0, std::string{"EFS1 16 8\n"}.size(), "EFS1 16 0\n");
        EXPECT_EQ(session.GetEnergyFieldState()->SerializeRegionalRecord(), record);
        ASSERT_TRUE(session.SaveWorld());
        EXPECT_EQ(ReadClockTestFile(session.GetWorldSaveDir() / "aether_state.efs"), record);
        GameSession reopened;
        EnableSnapshotSession(reopened, root, jobs);
        reopened.SetAetherStateEnabled(true);
        ASSERT_TRUE(reopened.LoadWorld(session.GetMetadata().worldId))
            << reopened.GetWorldOpenError();
        EXPECT_EQ(reopened.GetEnergyFieldState()->page_count(), pages);
        EXPECT_EQ(reopened.GetEnergyFieldState()->SerializeRegionalRecord(), record);
        EXPECT_EQ(reopened.GetActiveRegionLedger().energy_ages(),
                  session.GetActiveRegionLedger().energy_ages());
    }
}

void CheckDenseLegacyEnergyContinuation() {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession control, reopened;
    std::map<RegionKey, RegionState> owners{{{0, 0}, RegionState::Active}};
    auto record =
        LegacyEnergyCapacityRecord(luminumbra::fields::kEnergyWritePages, 2, true, owners);
    // 512 entries per page: 5010 cell-token bytes plus coordinates and newlines.
    ASSERT_EQ(record.size(), 20559675u);
    ASSERT_GT(record.size(), 16u * 1024u * 1024u);
    {
        EnergyState legacy(2);
        ASSERT_TRUE(legacy.DeserializeRecord(record, 31));
        EXPECT_TRUE(legacy.SerializeRecord(31) == record);
    }
    // One remaining tick means age 7 at tick 31, then a fire at tick 32.
    record.replace(7, 1, "1");
    const auto ledger = ScheduledFieldLedger(31, owners);
    {
        const std::optional<std::string> input{std::move(record)};
        ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(control, root, jobs, ledger, {}, input));
    }
    EXPECT_EQ(control.GetEnergyFieldState()->page_count(), 4096u);
    EXPECT_EQ(control.GetActiveRegionLedger().energy_ages().at({0, 0}), 7u);
    TickOnce(control);
    ASSERT_FALSE(control.GetRegionSchedulerFault());
    EXPECT_EQ(control.GetWorldClock().tick(), 32u);
    EXPECT_EQ(control.GetEnergyFieldState()->at_cell(0, 0, 0), 65023u);
    EXPECT_EQ(control.GetEnergyFieldState()->at_cell(0, 0, 1), 65023u);
    EXPECT_EQ(control.GetEnergyFieldState()->at_cell(16000, 0, 1), 65535u);
    ASSERT_TRUE(control.SaveWorld());
    const auto save = control.GetWorldSaveDir();
    {
        const auto expected = control.GetEnergyFieldState()->SerializeRegionalRecord();
        EXPECT_TRUE(ReadClockTestFile(save / "aether_state.efs") == expected);
        EnableSnapshotSession(reopened, root, jobs);
        reopened.SetAetherStateEnabled(true);
        ASSERT_TRUE(reopened.LoadWorld(control.GetMetadata().worldId))
            << reopened.GetWorldOpenError();
        reopened.SetReplicatedSimulationAnchors({});
        EXPECT_TRUE(reopened.GetEnergyFieldState()->SerializeRegionalRecord() == expected);
    }
    for (int tick = 0; tick < 8; ++tick) {
        TickOnce(control);
        TickOnce(reopened);
        ASSERT_FALSE(control.GetRegionSchedulerFault());
        ASSERT_FALSE(reopened.GetRegionSchedulerFault());
    }
    EXPECT_EQ(control.GetEnergyFieldState()->at_cell(0, 0, 0), 64515u);
    EXPECT_TRUE(reopened.GetEnergyFieldState()->SerializeRegionalRecord() ==
                control.GetEnergyFieldState()->SerializeRegionalRecord());
    EXPECT_EQ(reopened.GetActiveRegionLedger().encode(), control.GetActiveRegionLedger().encode());
    EXPECT_EQ(reopened.ComputeAetherStateSubHash(), control.ComputeAetherStateSubHash());
    ASSERT_TRUE(reopened.SaveWorld());
    EXPECT_TRUE(ReadClockTestFile(save / "aether_state.efs") ==
                control.GetEnergyFieldState()->SerializeRegionalRecord());
}

void CheckEnergyPublicationCopy(bool assignment, bool prepared) {
    const RegionKey owner{0, 0}, new_owner{4, 0};
    auto a =
        ScheduledFieldLedger(31, {{owner, RegionState::Active}, {new_owner, RegionState::Active}});
    ASSERT_TRUE(a.set_energy_age(owner, 3));
    const std::array original_work{Luminumbra::world::RegionWork{owner, 1, 0, 0, 0}};
    a.publish_completed_work(original_work);
    const auto before = a.encode();
    const std::array ages_a{std::pair{owner, std::uint64_t{7}},
                            std::pair{new_owner, std::uint64_t{5}}};
    const std::array ages_b{std::pair{owner, std::uint64_t{9}},
                            std::pair{new_owner, std::uint64_t{11}}};
    const std::array work_a{Luminumbra::world::RegionWork{owner, 2, 0, 0, 0}};
    const std::array work_b{Luminumbra::world::RegionWork{owner, 5, 0, 0, 0}};
    if (prepared) {
        ASSERT_TRUE(a.prepare_energy_work(ages_a, work_a));
    }
    const auto* same = &a;
    a = *same;
    auto b = a;
    if (assignment) {
        ASSERT_TRUE(b.prepare_energy_work(ages_b, work_b));
        b = a;
    }
    EXPECT_EQ(b.encode(), before);
    if (!prepared) {
        ASSERT_TRUE(a.prepare_energy_work(ages_a, work_a));
    }
    ASSERT_TRUE(b.prepare_energy_work(ages_b, work_b));
    a.commit_energy_work();
    EXPECT_EQ(a.energy_ages().at(owner), 7u);
    EXPECT_EQ(a.energy_ages().at(new_owner), 5u);
    EXPECT_EQ(a.completed_work().at(owner), 2u);
    EXPECT_EQ(b.encode(), before);
    b.commit_energy_work();
    EXPECT_EQ(b.energy_ages().at(owner), 9u);
    EXPECT_EQ(b.energy_ages().at(new_owner), 11u);
    EXPECT_EQ(b.completed_work().at(owner), 5u);
    EXPECT_EQ(a.energy_ages().at(owner), 7u);
    EXPECT_EQ(a.completed_work().at(owner), 2u);
}

void ReactivateEnergyPublicationSource(ActiveRegionLedger& source, const RegionKey& owner) {
    source.activate_owned(owner, WorldClock(31));
}

void CheckEnergyPublicationMove(bool assignment) {
    const RegionKey owner{0, 0};
    auto source = ScheduledFieldLedger(31, {{owner, RegionState::Active}});
    ASSERT_TRUE(source.set_energy_age(owner, 3));
    const auto before = source.encode();
    const std::array ages_a{std::pair{owner, std::uint64_t{7}}};
    const std::array ages_b{std::pair{owner, std::uint64_t{9}}};
    const std::array work_a{Luminumbra::world::RegionWork{owner, 2, 0, 0, 0}};
    const std::array work_b{Luminumbra::world::RegionWork{owner, 5, 0, 0, 0}};
    ASSERT_TRUE(source.prepare_energy_work(ages_a, work_a));
    auto moved = std::move(source);
    if (assignment) {
        auto destination = ScheduledFieldLedger(31, {{owner, RegionState::Active}});
        ASSERT_TRUE(destination.prepare_energy_work(ages_b, work_b));
        destination = std::move(moved);
        moved = std::move(destination);
    }
    EXPECT_EQ(moved.encode(), before);
    // Reusing the moved-from ledger must not alter the transferred preparation.
    ReactivateEnergyPublicationSource(source, owner);
    ASSERT_TRUE(source.prepare_energy_work(ages_b, work_b));
    moved.commit_energy_work();
    EXPECT_EQ(moved.energy_ages().at(owner), 7u);
    EXPECT_EQ(moved.completed_work().at(owner), 2u);
    source.commit_energy_work();
    EXPECT_EQ(source.energy_ages().at(owner), 9u);
    EXPECT_EQ(source.completed_work().at(owner), 5u);
    EXPECT_EQ(moved.energy_ages().at(owner), 7u);
    EXPECT_EQ(moved.completed_work().at(owner), 2u);
}

void CheckLegacyEnergyMigration(std::uint64_t remaining, bool empty, bool missing_distant = false) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession control, reopened;
    const RegionKey frozen{0, 0}, distant{30, 0};
    std::map<RegionKey, RegionState> owners{{frozen, RegionState::Frozen}};
    if (!missing_distant) {
        owners.emplace(distant, RegionState::Active);
    }
    const auto ledger = ScheduledFieldLedger(31, owners);
    const auto record = "EFS1 2 " + std::to_string(remaining) + "\n" +
                        (empty ? "" : "P 0 0 0:777\nP 40 0 0:60000\n");
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(control, root, jobs, ledger, {}, record));
    const auto frozen_bytes = control.GetEnergyFieldState()->RegionBytes(frozen);
    const auto initial_age = luminumbra::fields::kEnergyCadenceTicks - remaining;
    EXPECT_EQ(control.GetActiveRegionLedger().energy_ages().at(frozen), initial_age);
    EnableSnapshotSession(reopened, root, jobs);
    reopened.SetAetherStateEnabled(true);
    // Reopen before the first tick, either side of the first fire and later fires.
    const std::set<std::uint64_t> save_ticks{31, 32, 33, 34, 38, 39, 40, 48};
    for (std::uint64_t tick = 31; tick <= 64; ++tick) {
        SCOPED_TRACE(tick);
        if (save_ticks.contains(tick)) {
            ASSERT_TRUE(control.SaveWorld());
            ASSERT_TRUE(reopened.LoadWorld(control.GetMetadata().worldId))
                << reopened.GetWorldOpenError();
            reopened.SetReplicatedSimulationAnchors({});
        }
        for (auto* session : {&control, &reopened}) {
            const auto& ages = session->GetActiveRegionLedger().energy_ages();
            EXPECT_EQ(ages.at(distant), initial_age + tick - 31);
            EXPECT_EQ(ages.at(frozen), initial_age);
            EXPECT_EQ(session->GetEnergyFieldState()->RegionBytes(frozen), frozen_bytes);
            EXPECT_EQ(session->GetActiveRegionLedger().records().at(frozen).frozen_digest, 123u);
            if (empty) {
                EXPECT_EQ(session->GetEnergyFieldState()->page_count(), 0u);
            } else if (tick < 31 + remaining) {
                EXPECT_EQ(session->GetEnergyFieldState()->at_cell(640, 0), 60000u);
            } else {
                EXPECT_LT(session->GetEnergyFieldState()->at_cell(640, 0), 60000u);
            }
        }
        EXPECT_EQ(reopened.GetEnergyFieldState()->SerializeRegionalRecord(),
                  control.GetEnergyFieldState()->SerializeRegionalRecord());
        EXPECT_EQ(reopened.GetActiveRegionLedger().energy_ages(),
                  control.GetActiveRegionLedger().energy_ages());
        if (tick != 64) {
            TickOnce(control);
            TickOnce(reopened);
            ASSERT_FALSE(control.GetRegionSchedulerFault());
            ASSERT_FALSE(reopened.GetRegionSchedulerFault());
        }
    }
}

void CheckLegacyBoundaryEnergySaveReopen(bool in_world) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, reopened;
    const auto ledger = ScheduledFieldLedger(31, {{{0, 0}, RegionState::Frozen}});
    // Hand-written legacy input includes a wholly outside page and a boundary
    // page whose cells have owners -32769/-32768, plus the upper edge 32767/32768.
    const auto record = std::string("EFS1 2 3\nP -43692 0 0:999\n") +
                        (in_world ? "P -43691 0 8:111 9:222 10:12345 11:65535\n"
                                    "P 0 0 0:777 1:456\n"
                                    "P 43690 0 20:54321 21:9876 22:333 23:444\n"
                                  : "") +
                        "P 43691 0 0:888\n";
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, {}, record));
    const std::string expected = in_world ? "EFS1 2 0\nP -43691 0 10:12345 11:65535\n"
                                            "P 0 0 0:777 1:456\n"
                                            "P 43690 0 20:54321 21:9876\n"
                                          : "";
    ASSERT_EQ(session.GetEnergyFieldState()->SerializeRegionalRecord(), expected);
    const auto state = session.GetEnergyFieldState()->CanonicalRegionalBytes();
    const auto ages = session.GetActiveRegionLedger().energy_ages();
    ASSERT_TRUE(session.SaveWorld());
    const auto save = session.GetWorldSaveDir();
    if (in_world) {
        EXPECT_EQ(ReadClockTestFile(save / "aether_state.efs"), expected);
    } else {
        EXPECT_FALSE(fs::exists(save / "aether_state.efs"));
    }
    const auto digest = SessionWorldHash(session);
    EnableSnapshotSession(reopened, root, jobs);
    reopened.SetAetherStateEnabled(true);
    ASSERT_TRUE(reopened.LoadWorld(session.GetMetadata().worldId)) << reopened.GetWorldOpenError();
    EXPECT_EQ(reopened.GetEnergyFieldState()->CanonicalRegionalBytes(), state);
    EXPECT_EQ(reopened.GetEnergyFieldState()->SerializeRegionalRecord(), expected);
    EXPECT_EQ(reopened.GetActiveRegionLedger().energy_ages(), ages);
    ASSERT_TRUE(reopened.SaveWorld());
    EXPECT_EQ(SessionWorldHash(reopened), digest);
}

void CheckBoundaryEnergyEmitter(int x, int z) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    const int cx = static_cast<int>(std::floor(static_cast<double>(x) / 24));
    const int cz = static_cast<int>(std::floor(static_cast<double>(z) / 24));
    const auto owner = EnergyState::OwnerOfCell(cx, cz);
    auto ledger = ScheduledFieldLedger(31, {{owner, RegionState::Active}});
    ASSERT_TRUE(ledger.set_energy_age(owner, 0));
    ASSERT_NO_FATAL_FAILURE(
        LoadScheduledFieldFixture(session, root, jobs, ledger, {}, std::string{}));
    auto& registry = session.GetRegistry();
    const auto entity = registry.create();
    registry.emplace<C::TransformComponent>(entity).position =
        Luminumbra::Vec3(static_cast<float>(x), 0, static_cast<float>(z));
    registry.emplace<C::FieldEmitterComponent>(entity, 0, 1000u, 1);
    std::uint64_t expected = 0;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            const auto target = EnergyState::OwnerOfCell(cx + dx, cz + dz);
            if (target == owner) {
                expected += dx == 0 && dz == 0 ? 1000u : 500u;
            }
        }
    }
    for (std::uint64_t tick = 1; tick <= 3; ++tick) {
        TickOnce(session);
        ASSERT_FALSE(session.GetRegionSchedulerFault());
        EXPECT_EQ(session.GetEnergyFieldState()->total_raw(), expected * tick);
        EXPECT_EQ(session.GetEnergyFieldState()->at_cell(cx, cz), 1000u * tick);
        EXPECT_TRUE(session.GetEnergyFieldState()->ValidateRegionalOwners(
            session.GetActiveRegionLedger().records(),
            session.GetActiveRegionLedger().energy_ages()));
    }
}

void CheckRefusedEmitterBatchFaultsWithoutAdvancingEnergy() {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    const RegionKey owner{0, 0};
    auto ledger = ScheduledFieldLedger(31, {{owner, RegionState::Active}});
    ASSERT_TRUE(ledger.set_energy_age(owner, 0));
    ASSERT_NO_FATAL_FAILURE(
        LoadScheduledFieldFixture(session, root, jobs, ledger, {}, std::string{}));
    const auto entity = session.GetRegistry().create();
    session.GetRegistry().emplace<C::TransformComponent>(entity).position =
        Luminumbra::Vec3(0, 0, 0);
    // The session has two energy channels, so channel 7 makes the whole roster invalid.
    session.GetRegistry().emplace<C::FieldEmitterComponent>(entity, 7, 1000u, 1);
    const auto values = session.GetEnergyFieldState()->SerializeRegionalRecord();
    const auto ages = session.GetActiveRegionLedger().energy_ages();
    TickOnce(session);
    EXPECT_TRUE(session.GetRegionSchedulerFault());
    EXPECT_EQ(session.GetEnergyFieldState()->SerializeRegionalRecord(), values);
    EXPECT_EQ(session.GetActiveRegionLedger().energy_ages(), ages);
}

void CheckEnergyRegistersSurviveSchedulerRefusal() {
    const RegionKey owner{30, 0};
    auto ledger = ScheduledFieldLedger(31, {{owner, RegionState::Reduced}}, {0, 1, 2});
    ASSERT_TRUE(ledger.set_energy_age(owner, 7));
    const std::array work{Luminumbra::world::RegionWork{owner, 1, 0, 0, 0}};
    ledger.publish_completed_work(work);
    const auto before = ledger.encode();
    const auto* age = &ledger.energy_ages().at(owner);
    const auto* completed = &ledger.completed_work().at(owner);
    EXPECT_THROW(ledger.schedule(WorldClock(32),
                                 std::span<const Luminumbra::Vec3>{},
                                 {},
                                 [](RegionKey) -> std::uint64_t {
                                     throw std::runtime_error("injected digest refusal");
                                 }),
                 std::runtime_error);
    EXPECT_EQ(ledger.encode(), before);
    EXPECT_EQ(&ledger.energy_ages().at(owner), age);
    EXPECT_EQ(&ledger.completed_work().at(owner), completed);
    const auto scheduled = ledger.schedule(
        WorldClock(32), std::span<const Luminumbra::Vec3>{}, {}, [&](RegionKey key) {
            EXPECT_EQ(key, owner);
            EXPECT_EQ(ledger.encode(), before);
            return std::uint64_t{123};
        });
    EXPECT_EQ(scheduled.work, 1u);
    EXPECT_EQ(ledger.records().at(owner).state, RegionState::Frozen);
    EXPECT_EQ(&ledger.energy_ages().at(owner), age);
    EXPECT_EQ(ledger.energy_ages().at(owner), 7u);
    EXPECT_TRUE(ledger.completed_work().empty());
}

void CheckSessionEnergyRegisterReuse() {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    const RegionKey active{0, 0};
    std::map<RegionKey, RegionState> owners{{active, RegionState::Active}};
    std::string record = "EFS1 2 0\nP 0 0 0:60000\n";
    for (int page = 100; page < 4196; page += 4) {
        owners.emplace(EnergyState::OwnerOfCell(page * 16, 0), RegionState::Frozen);
        record += "P " + std::to_string(page) + " 0 0:777\n";
    }
    auto ledger = ScheduledFieldLedger(31, owners);
    for (const auto& [owner, state] : owners) {
        (void)state;
        ASSERT_TRUE(ledger.set_energy_age(owner, 0));
    }
    ASSERT_NO_FATAL_FAILURE(LoadScheduledFieldFixture(session, root, jobs, ledger, {}, record));
    for (int tick = 0; tick < 24; ++tick) {
        TickOnce(session);
        ASSERT_FALSE(session.GetRegionSchedulerFault());
    }
    const auto& live = session.GetActiveRegionLedger();
    const auto growths = live.energy_publication_growths();
    const auto* completed = &live.completed_work().at(active);
    std::map<RegionKey, const std::uint64_t*> registers;
    for (const auto& [owner, age] : live.energy_ages()) {
        registers.emplace(owner, &age);
    }
    const auto frozen = session.GetEnergyFieldState()->RegionBytes({75, 0});
    for (std::uint64_t age = 25; age <= 48; ++age) {
        TickOnce(session); // Includes scheduling, gathering, stepping and sparse publication.
        ASSERT_FALSE(session.GetRegionSchedulerFault());
        EXPECT_EQ(live.energy_publication_growths(), growths);
        EXPECT_EQ(live.energy_publication_age_visits(), 1u);
        EXPECT_EQ(live.energy_ages().at(active), age);
        ASSERT_EQ(live.completed_work().size(), 1u);
        EXPECT_EQ(&live.completed_work().at(active), completed);
        if (age % luminumbra::fields::kEnergyCadenceTicks != 0) {
            EXPECT_EQ(live.completed_work().at(active), 1u); // The due owner's age update.
        } else {
            EXPECT_GT(live.completed_work().at(active), 1u); // Age and processed pages.
        }
        // Stable node addresses catch scheduler copies even if the explicit growth hook misses one.
        for (const auto& [owner, address] : registers) {
            EXPECT_EQ(&live.energy_ages().at(owner), address);
            if (owner != active) {
                EXPECT_EQ(live.energy_ages().at(owner), 0u);
            }
        }
    }
    EXPECT_EQ(session.GetEnergyFieldState()->RegionBytes({75, 0}), frozen);
}

void CheckEnergyTransactionMatchesKernel(bool deposits, bool rollback) {
    auto ledger = ScheduledFieldLedger(0,
                                       {{{-1, 0}, RegionState::Active},
                                        {{0, 0}, RegionState::Active},
                                        {{1, 0}, RegionState::Reduced},
                                        {{4, 0}, RegionState::Frozen}});
    for (const auto& [owner, record] : ledger.records()) {
        (void)record;
        ASSERT_TRUE(ledger.set_energy_age(owner, 3));
    }
    EnergyState live(2);
    ASSERT_TRUE(live.DeserializeRecord("EFS1 2 8\nP -1 0 30:17000\nP 0 0 0:100 272:16000\n"
                                       "P 1 0 8:12000 10:500\nP 5 0 10:777\n",
                                       0));
    auto reference = live;
    EnergyState::RegionalTick transaction;
    for (std::uint64_t tick = 1; tick <= 32; ++tick) {
        SCOPED_TRACE(tick);
        std::vector<RegionKey> due;
        for (const auto& [owner, record] : ledger.records()) {
            if (record.state != RegionState::Frozen &&
                (tick & ((std::uint64_t{1} << record.cadence_shift) - 1)) == record.cadence_phase) {
                due.push_back(owner);
            }
        }
        auto& gathered = transaction.deposits(live.channels());
        if (deposits) {
            // New pages, saturation, a non-due target and a frozen target.
            for (const auto& [x, amount] : {std::pair{0, 100000u},
                                            std::pair{16, 700u},
                                            std::pair{21, 900u},
                                            std::pair{85, 1000u}}) {
                ASSERT_TRUE(gathered.QueueRegionalDeposit(1, x, 0, 0, amount));
                ASSERT_TRUE(reference.QueueRegionalDeposit(1, x, 0, 0, amount));
            }
            ASSERT_TRUE(gathered.QueueRegionalDeposit(2, 0, 16, 1, 900));
            ASSERT_TRUE(reference.QueueRegionalDeposit(2, 0, 16, 1, 900));
        }
        const auto before = live.SerializeRegionalRecord();
        const auto ages_before = ledger.energy_ages();
        const auto work_before = ledger.completed_work();
        ASSERT_TRUE(live.BeginRegionalTick(tick, ledger, due, transaction));
        if (rollback) {
            transaction.rollback();
            EXPECT_EQ(live.SerializeRegionalRecord(), before);
            EXPECT_EQ(ledger.energy_ages(), ages_before);
            EXPECT_EQ(ledger.completed_work(), work_before);
            ASSERT_TRUE(live.BeginRegionalTick(tick, ledger, due, transaction));
        }
        EnergyState::RegionalResult expected;
        ASSERT_TRUE(reference.TickRegional(tick, ledger.records(), due, ages_before, expected));
        EXPECT_EQ(live.SerializeRegionalRecord(), reference.SerializeRegionalRecord());
        ASSERT_TRUE(ledger.prepare_energy_work(transaction.ages(), transaction.work()));
        ledger.commit_energy_work();
        transaction.commit();
        EXPECT_EQ(ledger.energy_ages(), expected.ages);
        ActiveRegionLedger work_oracle = ledger;
        work_oracle.publish_completed_work(expected.work);
        EXPECT_EQ(ledger.completed_work(), work_oracle.completed_work());
        EXPECT_EQ(live.SerializeRegionalRecord(), reference.SerializeRegionalRecord());
    }
}

void CheckEnergyTransactionCost(bool deposits) {
    std::map<RegionKey, RegionState> owners{{{0, 0}, RegionState::Active}};
    std::string record = "EFS1 2 0\nP 0 0 0:60000\n";
    for (int page = 100; page < 4196; page += 4) {
        owners.emplace(EnergyState::OwnerOfCell(page * 16, 0), RegionState::Frozen);
        record += "P " + std::to_string(page) + " 0 0:777\n";
    }
    auto ledger = ScheduledFieldLedger(0, owners);
    for (const auto& [owner, state] : owners) {
        (void)state;
        ASSERT_TRUE(ledger.set_energy_age(owner, 0));
    }
    EnergyState live(2);
    ASSERT_TRUE(live.DeserializeRegionalRecord(record));
    const auto frozen = live.RegionBytes({75, 0});
    EnergyState::RegionalTick transaction;
    const std::array due{RegionKey{0, 0}};
    // Warm both an intrinsic fire and ordinary ticks, including recycled tree nodes.
    for (std::uint64_t tick = 1; tick <= 24; ++tick) {
        if (deposits) {
            ASSERT_TRUE(live.QueueRegionalDeposit(1, 0, 0, 0, 1000));
        }
        ASSERT_TRUE(live.BeginRegionalTick(tick, ledger, due, transaction));
        ASSERT_TRUE(ledger.prepare_energy_work(transaction.ages(), transaction.work()));
        ledger.commit_energy_work();
        transaction.commit();
    }
    const auto energy_growths = transaction.storage_growths();
    const auto ledger_growths = ledger.energy_publication_growths();
    for (std::uint64_t tick = 25; tick <= 48; ++tick) {
        SCOPED_TRACE(tick);
        if (deposits) {
            ASSERT_TRUE(live.QueueRegionalDeposit(1, 0, 0, 0, 1000));
        }
        ASSERT_TRUE(live.BeginRegionalTick(tick, ledger, due, transaction));
        EXPECT_EQ(transaction.participating_pages(), deposits || tick % 8 == 0 ? 1u : 0u);
        EXPECT_EQ(transaction.visited_pages(), transaction.participating_pages());
        ASSERT_TRUE(ledger.prepare_energy_work(transaction.ages(), transaction.work()));
        ledger.commit_energy_work();
        transaction.commit();
        EXPECT_EQ(transaction.storage_growths(), energy_growths);
        EXPECT_EQ(ledger.energy_publication_growths(), ledger_growths);
    }
    EXPECT_EQ(live.page_count(), 1025u);
    EXPECT_EQ(live.RegionBytes({75, 0}), frozen);
    EXPECT_EQ(ledger.energy_ages().at({75, 0}), 0u);
    EXPECT_EQ(ledger.energy_ages().at({0, 0}), 48u);
}

void CheckEnergyTransactionRefusals() {
    auto ledger = ScheduledFieldLedger(0,
                                       {{{0, 0}, RegionState::Active},
                                        {{2, 0}, RegionState::Active},
                                        {{4, 0}, RegionState::Frozen}});
    ASSERT_TRUE(ledger.set_energy_age({0, 0}, 7));
    EnergyState live(2);
    ASSERT_TRUE(live.DeserializeRegionalRecord("EFS1 2 0\nP 0 0 0:60000\n"));
    ASSERT_TRUE(live.QueueRegionalDeposit(1, 0, 0, 0, 1000));
    const auto before = live.SerializeRegionalRecord();
    const auto ages = ledger.energy_ages();
    const std::array due{RegionKey{0, 0}, RegionKey{2, 0}};
    EnergyState::RegionalTick transaction;
    for (const auto& invalid : {std::vector<RegionKey>{{0, 0}, {0, 0}},
                                std::vector<RegionKey>{{4, 0}},
                                std::vector<RegionKey>{{2, 0}, {0, 0}},
                                std::vector<RegionKey>{{99, 99}}}) {
        EXPECT_FALSE(live.BeginRegionalTick(1, ledger, invalid, transaction));
        EXPECT_EQ(live.SerializeRegionalRecord(), before);
        EXPECT_EQ(ledger.energy_ages(), ages);
    }
    EXPECT_FALSE(live.BeginRegionalTick(WorldClock::kTickLimit, ledger, due, transaction));
    EXPECT_EQ(live.SerializeRegionalRecord(), before);
    ASSERT_TRUE(live.BeginRegionalTick(1, ledger, due, transaction));
    const std::array bad_work{Luminumbra::world::RegionWork{{99, 99}, 1, 0, 0, 0}};
    EXPECT_FALSE(ledger.prepare_energy_work(transaction.ages(), bad_work));
    transaction.rollback();
    EXPECT_EQ(live.SerializeRegionalRecord(), before);
    EXPECT_EQ(ledger.energy_ages(), ages);
    // The manually queued input survived refusal and is consumed exactly once.
    auto reference = live;
    EnergyState::RegionalResult expected;
    ASSERT_TRUE(reference.TickRegional(1, ledger.records(), due, ages, expected));
    ASSERT_TRUE(live.BeginRegionalTick(1, ledger, due, transaction));
    ASSERT_TRUE(ledger.prepare_energy_work(transaction.ages(), transaction.work()));
    ledger.commit_energy_work();
    transaction.commit();
    EXPECT_EQ(live.SerializeRegionalRecord(), reference.SerializeRegionalRecord());
    EXPECT_EQ(ledger.energy_ages(), expected.ages);
    // Consumers can queue input after projection; commit retains it for the next tick.
    ASSERT_TRUE(live.BeginRegionalTick(2, ledger, due, transaction));
    ASSERT_TRUE(live.QueueRegionalDeposit(2, 0, 0, 0, 2000));
    ASSERT_TRUE(ledger.prepare_energy_work(transaction.ages(), transaction.work()));
    ledger.commit_energy_work();
    transaction.commit();
    reference = live;
    ASSERT_TRUE(reference.TickRegional(3, ledger.records(), due, ledger.energy_ages(), expected));
    ASSERT_TRUE(live.BeginRegionalTick(3, ledger, due, transaction));
    ASSERT_TRUE(ledger.prepare_energy_work(transaction.ages(), transaction.work()));
    ledger.commit_energy_work();
    transaction.commit();
    EXPECT_EQ(live.SerializeRegionalRecord(), reference.SerializeRegionalRecord());
    ASSERT_TRUE(ledger.set_energy_age({0, 0}, WorldClock::kTickLimit - 1));
    const auto settled = live.SerializeRegionalRecord();
    EXPECT_FALSE(live.BeginRegionalTick(4, ledger, due, transaction));
    EXPECT_EQ(live.SerializeRegionalRecord(), settled);
}
void CheckEnergyTransactionCapacityRefusal() {
    std::map<RegionKey, RegionState> owners{{{0, 0}, RegionState::Active}};
    std::string record = "EFS1 64 0\n";
    constexpr std::size_t page_bytes = 256u * 64u * sizeof(std::uint16_t);
    const auto limit = EnergyState::kMaxRegionalValueBytes / page_bytes;
    for (std::size_t index = 0; index < limit; ++index) {
        const auto page = static_cast<int>(index) * 4;
        owners.try_emplace(EnergyState::OwnerOfCell(page * 16, 0), RegionState::Frozen);
        record += "P " + std::to_string(page) + " 0 0:1\n";
    }
    auto ledger = ScheduledFieldLedger(0, owners);
    for (const auto& [owner, state] : owners) {
        (void)state;
        ASSERT_TRUE(ledger.set_energy_age(owner, 0));
    }
    EnergyState live(64);
    ASSERT_TRUE(live.DeserializeRegionalRecord(record));
    ASSERT_TRUE(live.QueueRegionalDeposit(1, 16, 0, 0, 1000));
    const auto ages = ledger.energy_ages();
    const std::array due{RegionKey{0, 0}};
    EnergyState::RegionalTick transaction;
    EXPECT_FALSE(live.BeginRegionalTick(1, ledger, due, transaction));
    EXPECT_EQ(live.SerializeRegionalRecord(), record);
    EXPECT_EQ(live.page_count(), limit);
    EXPECT_EQ(ledger.energy_ages(), ages);
    EXPECT_TRUE(ledger.completed_work().empty());
}

} // namespace

TEST(GameSessionHeadlessWorldTest, EnergyTransactionMatchesRegionalKernelAndRestoresRefusedTicks) {
    for (const bool deposits : {false, true}) {
        for (const bool rollback : {false, true}) {
            ASSERT_NO_FATAL_FAILURE(CheckEnergyTransactionMatchesKernel(deposits, rollback));
        }
    }
}

TEST(GameSessionHeadlessWorldTest, EnergyTransactionReusesStorageAndSkipsFrozenPages) {
    for (const bool deposits : {false, true}) {
        ASSERT_NO_FATAL_FAILURE(CheckEnergyTransactionCost(deposits));
    }
}

TEST(GameSessionHeadlessWorldTest, EnergyTransactionRefusesInvalidOwnersAndPublicationAtomically) {
    ASSERT_NO_FATAL_FAILURE(CheckEnergyTransactionRefusals());
}

TEST(GameSessionHeadlessWorldTest, EnergyTransactionCapacityRefusalLeavesValuesAndAgesUntouched) {
    ASSERT_NO_FATAL_FAILURE(CheckEnergyTransactionCapacityRefusal());
}

TEST(GameSessionHeadlessWorldTest, LegacyEnergyMigratesToOwnerAgesAndSealsFrozenOwners) {
    for (const std::uint64_t remaining : {1u, 3u, 8u}) {
        for (const bool empty : {false, true}) {
            ASSERT_NO_FATAL_FAILURE(CheckLegacyEnergyMigration(remaining, empty));
        }
    }
    ASSERT_NO_FATAL_FAILURE(CheckLegacyEnergyMigration(3, false, true));
}

TEST(GameSessionHeadlessWorldTest, LegacyBoundaryEnergyKeepsInWorldValuesAcrossSaveAndReopen) {
    for (const bool in_world : {false, true}) {
        ASSERT_NO_FATAL_FAILURE(CheckLegacyBoundaryEnergySaveReopen(in_world));
    }
}

TEST(GameSessionHeadlessWorldTest, LegacyEnergyCapacityAdmissionPreservesLiveStateOnRefusal) {
    for (const bool oversized : {true, false}) {
        ASSERT_NO_FATAL_FAILURE(CheckLegacyEnergyCapacityAdmission(oversized));
    }
}

TEST(GameSessionHeadlessWorldTest, DenseLegacyEnergyContinuesAcrossRegionalSaveAndReopen) {
    ASSERT_NO_FATAL_FAILURE(CheckDenseLegacyEnergyContinuation());
}

TEST(GameSessionHeadlessWorldTest, EnergyPublicationCopiesKeepIndependentPreparations) {
    for (const bool assignment : {false, true}) {
        for (const bool prepared : {false, true}) {
            ASSERT_NO_FATAL_FAILURE(CheckEnergyPublicationCopy(assignment, prepared));
        }
    }
}

TEST(GameSessionHeadlessWorldTest, EnergyPublicationMovesTransferIndependentPreparations) {
    for (const bool assignment : {false, true}) {
        ASSERT_NO_FATAL_FAILURE(CheckEnergyPublicationMove(assignment));
    }
}

TEST(GameSessionHeadlessWorldTest, RegionalEnergyClipsEmitterHaloAtWorldEdgesAndCorners) {
    for (const int x : {-16777216, 0, 16777200}) {
        for (const int z : {-16777216, 0, 16777200}) {
            ASSERT_NO_FATAL_FAILURE(CheckBoundaryEnergyEmitter(x, z));
        }
    }
}

TEST(GameSessionHeadlessWorldTest, RefusedEmitterBatchFaultsTheSchedulerWithoutAdvancingEnergy) {
    ASSERT_NO_FATAL_FAILURE(CheckRefusedEmitterBatchFaultsWithoutAdvancingEnergy());
}

TEST(GameSessionHeadlessWorldTest, SessionEnergyReusesRegistersAndPublishesOnlyParticipatingAges) {
    ASSERT_NO_FATAL_FAILURE(CheckSessionEnergyRegisterReuse());
}

TEST(GameSessionHeadlessWorldTest, EnergyRegistersStayTransactionalAcrossSchedulerRefusal) {
    ASSERT_NO_FATAL_FAILURE(CheckEnergyRegistersSurviveSchedulerRefusal());
}
