// asset-manifest split. The engine-side GameSession validates
// SIMULATION requirements only (preset and worldgen dependencies); renderer/UI
// asset requirements are caller-supplied by the client. Worldgen dependencies
// are simulation assets and must be available even for a headless host.
#include "../support/UniqueTempPath.h"
#include "../support/WorldgenTestData.h"

#include "../support/SystemConfigVariants.h"
#include "entt/entity/fwd.hpp"
#include "luminumbra/core/Types.h"
#include "luminumbra_common/components/CircadianComponents.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/CreatureComponents.h"
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
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "luminumbra_client/app/FarmSpeciesCache.h"
#include "luminumbra_client/app/SessionExit.h"
#include "luminumbra_client/rendering/TimeOfDayModel.h"
#include "luminumbra_common/ai/CircadianSystem.h"
#include "luminumbra_common/ai/CreatureSpeciesRegistry.h"
#include "luminumbra_common/ai/EcologyHash.h"
#include "luminumbra_common/ai/MigrationSystem.h"
#include "luminumbra_common/ai/StimulusChannels.h"
#include "luminumbra_common/components/CreatureComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/foliage/SpeciesRegistry.h"
#include "luminumbra_common/net/LockstepSession.h"
#include "luminumbra_common/persistence/SavedWorldCatalog.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/replay/RegionScheduleTrace.h"
#include "luminumbra_common/replay/ReplayStream.h"
#include "luminumbra_common/systems/AetherFieldSystem.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/PlantGrowthSystem.h"
#include "luminumbra_common/systems/WaterSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"

#include <nlohmann/json.hpp>

#include "luminumbra/core/Types.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/IrrigationComponents.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/core/WaterComponents.h"
#include "luminumbra_common/fields/EnergyFieldState.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/scripting/LuaState.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/FarAuthorityOverlay.h"
#include "luminumbra_common/world/FarLodStore.h"
#include "luminumbra_common/world/FarWantedSet.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/world/MarchingCubes.h"
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
        EXPECT_FALSE(session.LoadWorldStateFrom(save_dir));
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

void PreparePendingSnapshot(GameSession& session,
                            std::shared_ptr<Luminumbra::Chunk>& chunk,
                            bool existing_snapshot) {
    SCOPED_TRACE("prepare pending snapshot");
    const Luminumbra::IVec3 coords(0, 0, 0);
    // Fixed ticks can already have admitted this owner through the simulation horizon.
    chunk = session.GetWorldSystem()->find_streamed_chunk(coords);
    const bool resident = chunk != nullptr;
    if (!resident) {
        chunk = std::make_shared<Luminumbra::Chunk>(coords);
        chunk->set_state(Luminumbra::ChunkState::Idle);
    }
    chunk->heightmap_data = {8.0f, 9.0f, 10.0f};
    chunk->mark_voxel_data_dirty();
    if (!resident) {
        ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
    }
    ASSERT_EQ(session.GetWorldSystem()->find_streamed_chunk(coords), chunk);
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
    energy->Tick(0);
    if (existing_snapshot) {
        ASSERT_TRUE(session.SaveWorldState());
        ASSERT_FALSE(chunk->is_voxel_data_dirty());
    }

    chunk->heightmap_data.front() = 42.0f;
    chunk->mark_voxel_data_dirty();
    registry.get<C::PlantGrowthComponent>(plant).growth_points = 123;
    energy->QueueDeposit(2, 0, 0, 0, 1000);
    energy->Tick(0);
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
            session.SetAetherStateEnabled(true);
            ASSERT_TRUE(session.CreateWorld("Corrupt snapshot", "1337", "default"));
            std::shared_ptr<Luminumbra::Chunk> chunk;
            ASSERT_NO_FATAL_FAILURE(PreparePendingSnapshot(session, chunk, false));
            const PendingSnapshotState pending(session);
            const RetainedCreationState retained(session);
            const auto corrupt = Root().path() / "corrupt-state";
            ASSERT_TRUE(P::WorldSaveService::write_file_durably(corrupt / "world_info.json", "{"));
            ASSERT_FALSE(session.LoadWorldStateFrom(corrupt));
            ASSERT_FALSE(session.GetWorldOpenError().empty());
            pending.ExpectUnchanged(session, *chunk);
            retained.ExpectUnchanged(session);
            EXPECT_EQ(session.GetWorldSystem()->find_streamed_chunk(chunk->get_coords()), chunk);
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

INSTANTIATE_TEST_SUITE_P(ActiveRegionsEnabledAndDisabled, SavedWorldPresetTest, testing::Bool());

// Normalize historical text-mode energy records and current durable binary
// records for content assertions. Exact-byte assertions use ReadClockTestFile.
std::string ReadClockTestTextRecord(const fs::path& path) {
    std::string text = ReadClockTestFile(path);
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

nlohmann::json SessionWorldSubHashes(GameSession& session) {
    const auto chunk_hashes = session.GetWorldSystem()->compute_chunk_hashes();
    std::string aether = session.GetAetherFieldSystem()->ComputeAetherSubHash();
    const auto energy = session.ComputeAetherStateSubHash();
    if (!energy.empty())
        aether = P::StableChecksum(aether + "|state:" + energy);
    return {{"chunk", chunk_hashes.world},
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

TEST(GameSessionHeadlessWorldTest, DefaultAuthorityHashIgnoresStreamedCollisionNormalization) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, loaded;
    for (auto* current : {&session, &loaded}) {
        current->SetRootPath(root.root_string());
        current->SetJobSystem(&jobs);
    }
    ASSERT_TRUE(session.CreateWorld("Authority hash", "1337", "default"));
    ASSERT_TRUE(session.ActiveRegionsEnabled());
    const Luminumbra::IVec3 coords(0, 0, 0);
    auto edited = std::make_shared<Luminumbra::Chunk>(coords);
    edited->set_state(Luminumbra::ChunkState::Idle);
    edited->heightmap_data = {8.0f};
    edited->has_collision.store(true);
    edited->mark_voxel_data_dirty();
    ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(edited));
    const auto before = SessionWorldHash(session);
    ASSERT_TRUE(session.SaveWorldState());
    ASSERT_TRUE(loaded.LoadWorld(session.GetMetadata().worldId));
    const auto restored = loaded.GetWorldSystem()->find_streamed_chunk(coords);
    ASSERT_NE(restored, nullptr);
    EXPECT_FALSE(restored->has_collision.load());
    EXPECT_EQ(SessionWorldHash(loaded), before);

    // Extra generated cache residency and its collider do not become authority.
    auto natural = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(64, 0, 64));
    natural->set_state(Luminumbra::ChunkState::Idle);
    natural->heightmap_data = {12.0f};
    natural->has_collision.store(true);
    ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(natural));
    EXPECT_EQ(SessionWorldHash(session), before);
    edited->heightmap_data[0] = 9.0f;
    edited->mark_voxel_data_dirty();
    EXPECT_NE(SessionWorldHash(session), before);
}

TEST(GameSessionHeadlessWorldTest, DisabledRegionsKeepLegacyStreamedCollisionHash) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(session.CreateTransientWorld("Legacy hash", "1337", "default"));
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    chunk->set_state(Luminumbra::ChunkState::Idle);
    chunk->heightmap_data = {8.0f};
    ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
    Luminumbra::WorldStreamingState legacy;
    legacy.insert_chunk(chunk);
    const auto before = SessionWorldHash(session);
    // The historical contract includes streamed collider residency. Current R4
    // additionally binds the raw water scheduler even with regions disabled.
    EXPECT_EQ(session.GetWorldSystem()->compute_legacy_chunk_hashes().world,
              P::ComputeWorldStreamingStateHash(legacy));
    EXPECT_EQ(SessionWorldSubHashes(session).at("chunk"), "beb828adb58f9130");
    for (const bool collision : {false, true}) {
        chunk->has_collision.store(collision);
        for (const std::uint64_t cursor : {std::uint64_t{0}, std::uint64_t{0x01020304}}) {
            session.GetWorldSystem()->SetWaterSimWindowCursor(static_cast<std::size_t>(cursor));
            const auto historical = session.GetWorldSystem()->compute_legacy_chunk_hashes();
            EXPECT_EQ(historical.world, P::ComputeWorldStreamingStateHash(legacy));
            std::string scheduler("water_schedule:v1:");
            for (unsigned byte = 0; byte < 8; ++byte)
                scheduler.push_back(static_cast<char>((cursor >> (8 * byte)) & 0xffu));
            const auto water =
                P::StableChecksum("simulation_water_v5|grids:" +
                                  P::ComputeWorldStreamingStateSubHashes(legacy).water +
                                  "|schedule:" + P::StableChecksum(scheduler));
            const auto expected = P::StableChecksum(
                "simulation_authority_v5|base:" + P::ComputeWorldStreamingStateHash(legacy) +
                "|water:" + water);
            EXPECT_EQ(SessionWorldSubHashes(session).at("chunk"), expected);
            EXPECT_EQ(session.GetWorldSystem()->compute_chunk_hashes().sub.water, water);
        }
    }
    session.GetWorldSystem()->SetWaterSimWindowCursor(0);
    EXPECT_EQ(session.GetWorldSystem()->compute_legacy_chunk_hashes().world,
              P::ComputeWorldStreamingStateHash(legacy));
    EXPECT_EQ(SessionWorldSubHashes(session).at("chunk"), "ded16413f656b1ab");
    EXPECT_NE(SessionWorldHash(session), before);
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
    const Luminumbra::world::WorldCalendar calendar{120, 4};
    Luminumbra::world::WorldClock(0, calendar).write_metadata(metadata);
    ASSERT_TRUE(P::WorldSaveService::save_metadata(metadata.dump(), save));
    ASSERT_TRUE(original.LoadWorldStateFrom(save));
    ASSERT_TRUE(control.LoadWorldStateFrom(save));
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
        auto* energy = session->GetEnergyFieldState();
        energy->QueueDeposit(1, 0, 0, 0, 60000);
        energy->Tick(n);
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
    ASSERT_TRUE(fs::remove(missing_save / "aether_state.efs"));
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
        std::ofstream(save / "world_info.json", std::ios::binary) << metadata_bytes;
        std::ofstream(ledger_path, std::ios::binary) << ledger_bytes;
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
    session.SetReplicatedSimulationAnchors({});
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
    session.SetReplicatedSimulationAnchors({});
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

    // A failure between the two rewind replacements still leaves a readable pair.
    session.SetReplicatedSimulationAnchors({});
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_TRUE(session.SaveWorld());
    ASSERT_TRUE(session.LoadWorldStateFrom(alternate));
    P::WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    const bool interrupted_save = session.SaveWorld();
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
    const bool saved = session.SaveWorld();
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
            P::WorldSaveService::set_before_active_regions_replace_for_testing([]() -> bool {
                std::_Exit(73); // no destructors, unwinding or temporary-file cleanup
            });
            session.SaveWorld();
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
        if (entry.path().filename().string().find("active-regions.arl.tmp.") == 0)
            debris.push_back(entry.path());
    }
    ASSERT_EQ(debris.size(), 1u);
    EXPECT_EQ(debris.front().parent_path(), save);
    EXPECT_EQ(
        nlohmann::json::parse(ReadClockTestFile(save / "world_info.json")).at("simulationTick"),
        2u);
    Luminumbra::world::ActiveRegionLedger staged;
    std::string error;
    ASSERT_TRUE(Luminumbra::world::ActiveRegionLedger::decode(
        ReadClockTestFile(debris.front()), staged, error))
        << error;
    EXPECT_EQ(staged.tick(), 2u);
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

TEST(GameSessionHeadlessWorldTest, EmptyEnergyResumeAlignsFirstDepositWithoutReplayingHistory) {
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
    for (auto* session : {&control, &loaded, &explicit_load}) {
        auto* energy = session->GetEnergyFieldState();
        ASSERT_EQ(energy->page_count(), 0u);
        EXPECT_EQ(energy->next_fire_tick(), 320u);
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

TEST(GameSessionHeadlessWorldTest, EmptyLegacyEnergySnapshotPreservesNonAlignedCadence) {
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
    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(legacy.TickSimulation(dt), 1u);
    }
    legacy.GetEnergyFieldState()->QueueDeposit(1, 0, 0, 0, 1);
    legacy.GetEnergyFieldState()->Tick(5);
    ASSERT_TRUE(legacy.SaveWorldState());
    const auto save = legacy.GetWorldSaveDir();
    ASSERT_FALSE(nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"))
                     .contains("simulationTick"));
    ASSERT_TRUE(control.LoadWorld(legacy.GetMetadata().worldId));
    ASSERT_EQ(control.GetSimulationTickCount(), 0u);
    ASSERT_EQ(control.GetEnergyFieldState()->total_raw(), 1u);
    ASSERT_EQ(control.GetEnergyFieldState()->next_fire_tick(), 3u);
    for (int i = 0; i < 7; ++i) {
        ASSERT_EQ(control.TickSimulation(dt), 1u);
    }
    ASSERT_EQ(control.GetEnergyFieldState()->page_count(), 0u);
    ASSERT_EQ(control.GetEnergyFieldState()->next_fire_tick(), 11u);
    const auto explicit_save = root.path() / "empty-offset-snapshot";
    ASSERT_TRUE(control.SaveWorldStateTo(explicit_save));
    EXPECT_EQ(ReadClockTestTextRecord(explicit_save / "aether_state.efs"), "EFS1 2 4\n");
    ASSERT_TRUE(control.SaveWorldState());
    EXPECT_EQ(ReadClockTestTextRecord(save / "aether_state.efs"), "EFS1 2 4\n");
    ASSERT_TRUE(loaded.LoadWorld(control.GetMetadata().worldId));
    ASSERT_TRUE(explicit_load.CreateTransientWorld("Explicit", "1337", "default"));
    ASSERT_TRUE(explicit_load.LoadWorldStateFrom(explicit_save));
    for (auto* session : {&control, &loaded, &explicit_load}) {
        auto* energy = session->GetEnergyFieldState();
        ASSERT_EQ(energy->page_count(), 0u);
        EXPECT_EQ(energy->next_fire_tick(), 11u);
        energy->QueueDeposit(1, 0, 0, 0, 60000);
    }
    for (int tick = 8; tick <= 15; ++tick) {
        SCOPED_TRACE(tick);
        for (auto* session : {&control, &loaded, &explicit_load}) {
            ASSERT_EQ(session->TickSimulation(dt), 1u);
            if (tick < 11) {
                EXPECT_EQ(session->GetEnergyFieldState()->at_cell(0, 0), 60000u);
            } else {
                EXPECT_LT(session->GetEnergyFieldState()->at_cell(0, 0), 60000u);
            }
        }
        EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(control));
        EXPECT_EQ(SessionWorldHash(explicit_load), SessionWorldHash(control));
    }
}

TEST(GameSessionHeadlessWorldTest, EmptyAlignedEnergySaveRemovesStaleRecord) {
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
    EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(original));
}

TEST(GameSessionHeadlessWorldTest, EnergyWriteFailureRefusesBothRegionModesAndDirtyPaths) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    for (const bool active_regions : {false, true}) {
        for (const bool dirty : {false, true}) {
            SCOPED_TRACE(active_regions);
            SCOPED_TRACE(dirty);
            GameSession session;
            session.SetRootPath(root.root_string());
            session.SetJobSystem(&jobs);
            session.SetActiveRegionsEnabled(active_regions);
            session.SetAetherStateEnabled(true);
            ASSERT_TRUE(session.CreateWorld("Failed energy save", "1337", "default"));
            auto* energy = session.GetEnergyFieldState();
            ASSERT_NE(energy, nullptr);
            energy->SetAnchorCell(0, 0);
            energy->QueueDeposit(1, 0, 0, 0, 60000);
            energy->Tick(0);
            ASSERT_GT(energy->page_count(), 0u);
            if (dirty) {
                const auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
                chunk->heightmap_data = {8.0f};
                chunk->set_state(Luminumbra::ChunkState::Idle);
                chunk->mark_voxel_data_dirty();
                ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
            }
            const auto save = session.GetWorldSaveDir();
            const auto obstruction = save / "aether_state.efs";
            ASSERT_TRUE(fs::create_directory(obstruction));
            std::ofstream(obstruction / "retained") << "retained";
            const auto metadata = ReadClockTestFile(save / "world_info.json");
            const auto before_energy = energy->CanonicalBytes();
            Luminumbra::world::WorldStateSaveReport report;
            EXPECT_FALSE(session.SaveWorldState(&report));
            EXPECT_FALSE(report.saved);
            EXPECT_EQ(energy->CanonicalBytes(), before_energy);
            EXPECT_EQ(ReadClockTestFile(save / "world_info.json"), metadata);
            EXPECT_EQ(ReadClockTestFile(obstruction / "retained"), "retained");
            ASSERT_EQ(fs::remove_all(obstruction), 2u);
            ASSERT_TRUE(session.SaveWorldState());
            EXPECT_TRUE(fs::is_regular_file(obstruction));
        }
    }
}

TEST(GameSessionHeadlessWorldTest, InterruptedEnergyReplacementRetainsPreviousPayload) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(false);
    session.SetAetherStateEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Energy replacement", "1337", "default"));
    auto* energy = session.GetEnergyFieldState();
    ASSERT_NE(energy, nullptr);
    energy->SetAnchorCell(0, 0);
    energy->QueueDeposit(1, 0, 0, 0, 1000);
    energy->Tick(0);
    ASSERT_TRUE(session.SaveWorldState());
    const auto save = session.GetWorldSaveDir();
    const auto before = SavedPresetTestDiskBytes(save);
    energy->QueueDeposit(1, 0, 0, 0, 1000);
    energy->Tick(0);
    const auto live = energy->CanonicalBytes();
    P::WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    const bool saved = session.SaveWorldState();
    P::WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
    EXPECT_FALSE(saved);
    EXPECT_EQ(SavedPresetTestDiskBytes(save), before);
    EXPECT_EQ(energy->CanonicalBytes(), live);
    ASSERT_TRUE(session.SaveWorldState());
    EXPECT_NE(ReadClockTestFile(save / "aether_state.efs"), before.at("aether_state.efs"));
}

TEST(GameSessionHeadlessWorldTest, InterruptedPlantReplacementRetainsPreviousPayload) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(false);
    session.SetAetherStateEnabled(false);
    ASSERT_TRUE(session.CreateWorld("Plant replacement", "1337", "default"));
    const auto plant = ClockTestPlant(session.GetRegistry(), session.GetMetadata().spawnPoint);
    ASSERT_TRUE(session.SaveWorldState());
    const auto save = session.GetWorldSaveDir();
    const auto before = SavedPresetTestDiskBytes(save);
    ++session.GetRegistry().get<C::PlantGrowthComponent>(plant).growth_points;
    const auto live = session.ComputePlantSubHash();
    P::WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    const bool saved = session.SaveWorldState();
    P::WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
    EXPECT_FALSE(saved);
    EXPECT_EQ(SavedPresetTestDiskBytes(save), before);
    EXPECT_EQ(session.ComputePlantSubHash(), live);
    ASSERT_TRUE(session.SaveWorldState());
    EXPECT_NE(SavedPresetTestDiskBytes(save), before);
}

TEST(GameSessionHeadlessWorldTest, OpenPlantPayloadRemovalUsesPlatformFileSemantics) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    for (const bool active_regions : {false, true}) {
        SCOPED_TRACE(active_regions);
        GameSession session;
        session.SetRootPath(root.root_string());
        session.SetJobSystem(&jobs);
        session.SetActiveRegionsEnabled(active_regions);
        ASSERT_TRUE(session.CreateWorld("Opened plant removal", "1337", "default"));
        const auto plant = ClockTestPlant(session.GetRegistry(), session.GetMetadata().spawnPoint);
        ASSERT_TRUE(session.SaveWorldState());
        const auto save = session.GetWorldSaveDir();
        const auto payload = P::WorldSaveService::plant_entities_path(save);
        session.GetRegistry().destroy(plant);
        // The CRT reader permits other readers, but denies deletion on Windows.
        // POSIX unlinks the path while this opened image remains readable.
        std::ifstream opened(payload, std::ios::binary);
        ASSERT_TRUE(opened.is_open());
        const auto before = SavedPresetTestDiskBytes(save);
#if defined(_WIN32) && !defined(__CYGWIN__)
        EXPECT_FALSE(session.SaveWorldState());
        EXPECT_EQ(SavedPresetTestDiskBytes(save), before);
#else
        EXPECT_TRUE(session.SaveWorldState());
        EXPECT_FALSE(fs::exists(payload));
#endif
        std::ostringstream retained;
        retained << opened.rdbuf();
        EXPECT_EQ(retained.str(), before.at("chunks/region/plant-entities.json"));
        opened.close();
        ASSERT_TRUE(session.SaveWorldState());
        EXPECT_FALSE(fs::exists(payload));
    }
}

TEST(GameSessionHeadlessWorldTest, InvalidEnergyRecordsRefuseBothLoadEntriesAndDisableSaving) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    for (const bool active_regions : {false, true}) {
        SCOPED_TRACE(active_regions);
        GameSession source;
        source.SetRootPath(root.root_string());
        source.SetJobSystem(&jobs);
        source.SetActiveRegionsEnabled(active_regions);
        source.SetAetherStateEnabled(true);
        ASSERT_TRUE(source.CreateWorld("Invalid energy", "1337", "default"));
        ClockTestPlant(source.GetRegistry(), source.GetMetadata().spawnPoint);
        ASSERT_TRUE(source.SaveWorldState());
        const auto save = source.GetWorldSaveDir();
        const auto payload = save / "aether_state.efs";
        for (const char* record : {"",
                                   "broken",
                                   "EFS1",
                                   "EFS1 +",
                                   "EFS1 2 0\n",
                                   "EFS1 2 8\nP 0\n",
                                   "EFS1 1 8\n",
                                   "EFS1 2147483647 8\nP 0 0\n"}) {
            SCOPED_TRACE(record);
            std::ofstream(payload, std::ios::binary | std::ios::trunc) << record;
            const auto before = SavedPresetTestDiskBytes(save);
            for (const bool full_open : {false, true}) {
                SCOPED_TRACE(full_open);
                GameSession loaded;
                loaded.SetRootPath(root.root_string());
                loaded.SetJobSystem(&jobs);
                loaded.SetActiveRegionsEnabled(active_regions);
                loaded.SetAetherStateEnabled(true);
                if (full_open) {
                    EXPECT_FALSE(loaded.LoadWorld(source.GetMetadata().worldId));
                } else {
                    ASSERT_TRUE(loaded.CreateTransientWorld("Energy reload", "1337", "default"));
                    EXPECT_FALSE(loaded.LoadWorldStateFrom(save));
                }
                EXPECT_EQ(loaded.GetWorldOpenError(), "Corrupt or unreadable aether state record.");
                EXPECT_TRUE(loaded.GetWorldSystem()->snapshot_streamed_chunks().empty());
                EXPECT_TRUE(loaded.GetRegistry().view<C::PlantTag>().empty());
                EXPECT_FALSE(loaded.SaveWorldStateTo(save));
                EXPECT_EQ(SavedPresetTestDiskBytes(save), before);
            }
        }
        ASSERT_TRUE(fs::remove(payload));
        ASSERT_TRUE(fs::create_directory(payload));
        const auto before = SavedPresetTestDiskBytes(save);
        EXPECT_FALSE(source.LoadWorldStateFrom(save));
        EXPECT_EQ(source.GetWorldOpenError(), "Corrupt or unreadable aether state record.");
        EXPECT_FALSE(source.SaveWorldState());
        EXPECT_EQ(SavedPresetTestDiskBytes(save), before);
    }
}

TEST(GameSessionHeadlessWorldTest, MissingEnergyRecordClearsOldStateAndDisabledLayerIgnoresData) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    for (const bool active_regions : {false, true}) {
        GameSession source;
        source.SetRootPath(root.root_string());
        source.SetJobSystem(&jobs);
        source.SetActiveRegionsEnabled(active_regions);
        source.SetAetherStateEnabled(true);
        ASSERT_TRUE(source.CreateWorld("Absent energy", "1337", "default"));
        auto* energy = source.GetEnergyFieldState();
        ASSERT_NE(energy, nullptr);
        energy->SetAnchorCell(0, 0);
        energy->QueueDeposit(1, 0, 0, 0, 1000);
        energy->Tick(0);
        ASSERT_GT(energy->page_count(), 0u);
        const auto save = source.GetWorldSaveDir();
        ASSERT_FALSE(fs::exists(save / "aether_state.efs"));
        ASSERT_TRUE(source.LoadWorldStateFrom(save));
        EXPECT_EQ(energy->page_count(), 0u);
        std::ofstream(save / "aether_state.efs", std::ios::binary)
            << " \tEFS1 +2 8\r\nP 0 0 0:1000\r\n";
        GameSession historical;
        historical.SetRootPath(root.root_string());
        historical.SetJobSystem(&jobs);
        historical.SetActiveRegionsEnabled(active_regions);
        historical.SetAetherStateEnabled(true);
        ASSERT_TRUE(historical.LoadWorld(source.GetMetadata().worldId));
        ASSERT_NE(historical.GetEnergyFieldState(), nullptr);
        EXPECT_EQ(historical.GetEnergyFieldState()->total_raw(), 1000u);
        std::ofstream(save / "aether_state.efs", std::ios::binary) << "broken";
        GameSession disabled;
        disabled.SetRootPath(root.root_string());
        disabled.SetJobSystem(&jobs);
        disabled.SetActiveRegionsEnabled(active_regions);
        disabled.SetAetherStateEnabled(false);
        ASSERT_TRUE(disabled.LoadWorld(source.GetMetadata().worldId));
        EXPECT_EQ(disabled.GetEnergyFieldState(), nullptr);
        EXPECT_EQ(ReadClockTestFile(save / "aether_state.efs"), "broken");
    }
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
            auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(64, 0, 64));
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
    original.SetReplicatedSimulationAnchors({});
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
    loaded.SetReplicatedSimulationAnchors({});
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
    const auto before = ReadClockTestFile(path);
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
    EXPECT_EQ(before, legacy.dump(4) + "\n");
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
    };
    for (const auto pin : {Pin{30, .25f, .0625f, .575f, .125f},
                           Pin{60, .5f, .125f, 1.0f, .25f},
                           Pin{120, 0.0f, .25f, .15f, .5f},
                           Pin{240, 0.0f, .5f, .15f, 1.0f},
                           Pin{360, 0.0f, .75f, .15f, .5f},
                           Pin{480, 0.0f, 0.0f, .15f, 0.0f}}) {
        SCOPED_TRACE(pin.tick);
        Luminumbra::world::WorldClock(pin.tick - 1, {120, 4}).write_metadata(metadata);
        ASSERT_TRUE(P::WorldSaveService::save_metadata(metadata.dump(), save));
        ASSERT_TRUE(session.LoadWorld(id)) << session.GetWorldOpenError();
        const auto position = session.GetMetadata().spawnPoint;
        const auto plant = ClockTestPlant(session.GetRegistry(), position);
        const auto creature = ClockTestCreature(session.GetRegistry());
        ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
        const auto& clock = session.GetWorldClock();
        EXPECT_FLOAT_EQ(static_cast<float>(clock.day_phase(pin.tick)), pin.day);
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
        ASSERT_TRUE(
            fs::remove(P::WorldSaveService::active_regions_path(original.GetWorldSaveDir())));
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

void ChangeRegionResumeHeight(std::initializer_list<GameSession*> sessions,
                              const Luminumbra::IVec3& coords) {
    // The durable copy from tick three must lose to newer resident authority.
    for (auto* session : sessions) {
        const auto chunk = session->GetWorldSystem()->find_streamed_chunk(coords);
        ASSERT_NE(chunk, nullptr);
        ASSERT_EQ(chunk->get_id(), Luminumbra::Chunk::calculate_id(coords));
        ASSERT_FALSE(chunk->heightmap_data.empty());
        chunk->heightmap_data[0] = 12.0f;
        chunk->mark_voxel_data_dirty();
        int rx = 0, rz = 0;
        P::WorldSaveService::region_coords_for_chunk(coords, rx, rz);
        const auto live = session->GetWorldSystem()->snapshot_region_authority(rx, rz);
        ASSERT_EQ(live.size(), 1u);
        ASSERT_EQ(live.front(), chunk);
        P::RegionContentHasher expected;
        expected.append(*chunk);
        EXPECT_EQ(P::WorldSaveService::region_simulation_digest(
                      session->GetWorldSystem()->authority_directory(),
                      {rx, rz},
                      P::RegionDigestInputs{live, {}, true}),
                  expected.value());
    }
}

TEST(GameSessionHeadlessWorldTest, ResidentRegionDigestKeepsSignedBoundariesAndLivePrecedence) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateTransientWorld("Resident digest", "1337", "default"));
    auto* world = session.GetWorldSystem();
    for (const int x : {-1281, -1280, -1, 0, 31, 32, 1280}) {
        for (const int z : {-1281, -1280, -1, 0, 31, 32, 1280}) {
            const Luminumbra::IVec3 coords(x, 20, z);
            const auto chunk = std::make_shared<Luminumbra::Chunk>(coords);
            chunk->set_state(Luminumbra::ChunkState::Idle);
            chunk->heightmap_data = {8.0f, 8.5f, 9.0f};
            chunk->mark_voxel_data_dirty();
            ASSERT_TRUE(world->adopt_streamed_chunk(chunk));
        }
    }
    const Luminumbra::IVec3 target(1280, 20, -1280);
    const Luminumbra::world::RegionKey key{40, -40};
    const auto chunk = world->find_streamed_chunk(target);
    ASSERT_NE(chunk, nullptr);
    ASSERT_TRUE(P::WorldSaveService::persist_chunk_authority(world->authority_directory(), chunk));
    chunk->heightmap_data[0] = 12.0f;
    chunk->mark_voxel_data_dirty();
    for (const auto state : {Luminumbra::ChunkState::Loading,
                             Luminumbra::ChunkState::Idle,
                             Luminumbra::ChunkState::Meshing,
                             Luminumbra::ChunkState::Ready}) {
        chunk->set_state(state);
        const auto live = world->snapshot_region_authority(key.x, key.z);
        ASSERT_EQ(live.size(), 1u);
        EXPECT_EQ(live.front(), chunk);
        P::RegionContentHasher expected;
        expected.append(*chunk);
        for (const auto& directory : {fs::path{}, world->authority_directory()}) {
            EXPECT_EQ(P::WorldSaveService::region_simulation_digest(
                          directory, key, P::RegionDigestInputs{live, {}, true}),
                      expected.value());
        }
    }
    for (const auto& resident : world->snapshot_streamed_chunks()) {
        const auto coords = resident->get_coords();
        const auto floor_region = [](int value) {
            return static_cast<int>(std::floor(static_cast<double>(value) / 32.0));
        };
        const auto live =
            world->snapshot_region_authority(floor_region(coords.x), floor_region(coords.z));
        EXPECT_NE(std::find(live.begin(), live.end(), resident), live.end());
    }
    EXPECT_TRUE(world->snapshot_region_authority(100, 100).empty());
}

void ExpectFrozenRegionEditedDigest(GameSession& original,
                                    GameSession& uninterrupted,
                                    Luminumbra::world::RegionKey far,
                                    const Luminumbra::IVec3& coords) {
    ASSERT_EQ(original.GetActiveRegionLedger().records().at(far).state,
              Luminumbra::world::RegionState::Frozen);
    EXPECT_EQ(original.GetActiveRegionLedger().records().at(far).frozen_digest,
              uninterrupted.GetActiveRegionLedger().records().at(far).frozen_digest);
    const auto edited = original.GetWorldSystem()->find_streamed_chunk(coords);
    ASSERT_NE(edited, nullptr);
    ASSERT_FALSE(edited->heightmap_data.empty());
    ASSERT_EQ(edited->heightmap_data.front(), 12.0f);
    P::RegionContentHasher expected;
    expected.append(*edited);
    EXPECT_EQ(original.GetActiveRegionLedger().records().at(far).frozen_digest, expected.value());
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
            ASSERT_NO_FATAL_FAILURE(ChangeRegionResumeHeight(
                {&original, &uninterrupted, &render_only, &loaded}, coords));
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
            ASSERT_NO_FATAL_FAILURE(
                ExpectFrozenRegionEditedDigest(original, uninterrupted, far, coords));
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
            const auto before = SessionWorldHash(original);
            ASSERT_TRUE(original.SaveWorldState());
            EXPECT_EQ(SessionWorldHash(original), before);
            ASSERT_TRUE(render_only.SaveWorldState());
            EXPECT_EQ(ReadClockTestFile(
                          P::WorldSaveService::active_regions_path(original.GetWorldSaveDir())),
                      ReadClockTestFile(
                          P::WorldSaveService::active_regions_path(render_only.GetWorldSaveDir())));
            ASSERT_TRUE(loaded.LoadWorld(original.GetMetadata().worldId))
                << loaded.GetWorldOpenError();
            EXPECT_EQ(loaded.GetActiveRegionLedger().encode(),
                      original.GetActiveRegionLedger().encode());
            EXPECT_EQ(SessionWorldHash(loaded), before);
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
        save, key, P::RegionDigestInputs{std::move(live), std::move(parked)});
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
    EXPECT_EQ(loaded.GetWorldSystem()->snapshot_streamed_chunks().size(), 2u);
    EXPECT_EQ(SessionWorldHash(loaded), before);
}

TEST(GameSessionHeadlessWorldTest, ExplicitFullSnapshotHandlesAnEmptyCleanWorld) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Empty full snapshot", "1337", "default"));
    ASSERT_TRUE(session.GetWorldSystem()->snapshot_streamed_chunks().empty());
    const auto save = session.GetWorldSaveDir();
    ASSERT_TRUE(session.SaveWorldState(nullptr, true));
    EXPECT_TRUE(fs::exists(P::WorldSaveService::world_manifest_path(save)));
    EXPECT_FALSE(P::WorldSaveService::has_chunk_snapshot(save));
    const auto before = ReadClockTestFile(save / "world_info.json");
    P::WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    const bool saved = session.SaveWorldState(nullptr, true);
    P::WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
    EXPECT_FALSE(saved);
    EXPECT_EQ(ReadClockTestFile(save / "world_info.json"), before);
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

TEST(GameSessionHeadlessWorldTest, IdleRetentionUsesWorkRegardlessOfStreamedResidency) {
    using Luminumbra::world::ActiveRegionLedger;
    using Luminumbra::world::RegionKey;
    using Luminumbra::world::RegionWork;
    using Luminumbra::world::WorldClock;
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session, peer;
    for (auto* current : {&session, &peer}) {
        current->SetRootPath(root.root_string());
        current->SetJobSystem(&jobs);
        current->SetActiveRegionsEnabled(true);
    }
    ASSERT_TRUE(session.CreateWorld("Idle retention", "1337", "default"));
    ASSERT_TRUE(session.SaveWorldState());
    ASSERT_TRUE(peer.CreateTransientWorld("Idle peer", "1337", "default"));

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
    for (auto* current : {&session, &peer}) {
        ASSERT_TRUE(current->LoadWorldStateFrom(save)) << current->GetWorldOpenError();
        ASSERT_EQ(current->GetActiveRegionLedger().records().size(), keys.size());
        current->SetReplicatedSimulationAnchors({});
    }

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
        // Both peers have identical authoritative edits; only this session has
        // the extra pristine residents, including an in-flight Loading cache.
        if (key == resident || key == loading) {
            auto copy = std::make_shared<Luminumbra::Chunk>(chunk->get_coords());
            copy->set_state(Luminumbra::ChunkState::Idle);
            copy->heightmap_data = chunk->heightmap_data;
            copy->mark_voxel_data_dirty();
            ASSERT_TRUE(peer.GetWorldSystem()->adopt_streamed_chunk(copy));
        }
        ASSERT_FALSE(session.GetActiveRegionLedger().records().at(key).edited);
    }
    const std::array work{RegionWork{pending, 3, 0, 0, 0},
                          RegionWork{duplicate, 1, 0, 0, 0},
                          RegionWork{duplicate, 2, 0, 0, 0},
                          RegionWork{zero_count, 0, 0, 0, 0},
                          RegionWork{zero_count, 0, 0, 0, 0}};
    for (auto* current : {&session, &peer})
        for (const auto& entry : work)
            current->RecordActiveRegionWork(entry);
    auto expected = session.GetActiveRegionLedger();
    const std::array owned_keys{pending, duplicate, zero_count};
    const auto expected_schedule = expected.schedule(
        WorldClock(ActiveRegionLedger::kIdleEvictionTicks),
        std::span<const Luminumbra::Vec3>{},
        work,
        {},
        [&owned_keys](RegionKey key) {
            return std::find(owned_keys.begin(), owned_keys.end(), key) != owned_keys.end();
        });
    const auto dt = session.GetSimulationClock().fixed_dt();
    for (auto* current : {&session, &peer})
        ASSERT_EQ(current->TickSimulation(dt), 1u);
    EXPECT_EQ(session.GetActiveRegionLedger().canonical_bytes(),
              peer.GetActiveRegionLedger().canonical_bytes());
    EXPECT_EQ(
        session.FoldClockIntoEcologyHash(
            luminumbra::ai::ComputeEcologySubHash(session.GetRegistry())),
        peer.FoldClockIntoEcologyHash(luminumbra::ai::ComputeEcologySubHash(peer.GetRegistry())));
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
    const auto next_schedule =
        expected.schedule(WorldClock(ActiveRegionLedger::kIdleEvictionTicks + 1),
                          std::span<const Luminumbra::Vec3>{},
                          {},
                          {},
                          [](RegionKey) { return false; });
    for (auto* current : {&session, &peer})
        ASSERT_EQ(current->TickSimulation(dt), 1u);
    EXPECT_EQ(session.GetActiveRegionLedger().canonical_bytes(),
              peer.GetActiveRegionLedger().canonical_bytes());
    EXPECT_EQ(
        session.FoldClockIntoEcologyHash(
            luminumbra::ai::ComputeEcologySubHash(session.GetRegistry())),
        peer.FoldClockIntoEcologyHash(luminumbra::ai::ComputeEcologySubHash(peer.GetRegistry())));
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_EQ(session.GetRegionSchedule(), next_schedule);
    EXPECT_EQ(session.GetActiveRegionLedger().canonical_bytes(), expected.canonical_bytes());
    EXPECT_EQ(session.GetActiveRegionLedger().encode(), expected.encode());
    EXPECT_TRUE(session.GetActiveRegionLedger().records().empty());
}

TEST(GameSessionHeadlessWorldTest, FrozenDigestSurvivesDurableCameraEviction) {
    using Luminumbra::Chunk;
    using Luminumbra::ChunkState;
    using Luminumbra::IVec3;
    using Luminumbra::Vec3;
    using Luminumbra::world::RegionKey;
    using Luminumbra::world::RegionState;
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession resident, moved, loaded;
    for (auto* session : {&resident, &moved, &loaded}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetActiveRegionsEnabled(true);
        session->SetRegionSchedulerConfig({0, 1, 1});
    }
    ASSERT_TRUE(resident.CreateWorld("Resident edits", "1337", "default"));
    ASSERT_TRUE(moved.CreateWorld("Parked edits", "1337", "default"));
    const RegionKey key{-40, -40};
    const float x = static_cast<float>(key.x * 512 + 24);
    const float z = static_cast<float>(key.z * 512 + 24);
    const float height = resident.GetWorldSystem()->GetTerrainHeightAt(x, z);
    const IVec3 coords(
        key.x * 32 + 1, static_cast<int>(std::floor(height / 16.0f)), key.z * 32 + 1);
    const Vec3 edit_anchor(x, static_cast<float>(coords.y * 16 + 8), z);
    std::array<std::shared_ptr<Chunk>, 2> chunks;
    for (std::size_t index = 0; index < chunks.size(); ++index) {
        auto* session = index == 0 ? &resident : &moved;
        auto& chunk = chunks[index];
        chunk = std::make_shared<Chunk>(coords);
        chunk->set_state(ChunkState::Idle);
        chunk->sdf_data.assign(std::size_t{17} * 17 * 17, 1.0f);
        chunk->heightmap_data.assign(std::size_t{17} * 17, 8.0f);
        chunk->mark_voxel_data_dirty();
        ASSERT_TRUE(session->GetWorldSystem()->adopt_streamed_chunk(chunk));
        session->PinActiveRegion(key, true);
        session->SetReplicatedSimulationAnchors({});
        ASSERT_TRUE(session->SaveWorldState());
        // The current authoritative edit must override the stale durable image.
        chunk->heightmap_data[0] = 12.0f;
        chunk->mark_voxel_data_dirty();
        // Complete authority remains valid during a render lifecycle transition.
        chunk->set_state(ChunkState::Loading);
    }
    const auto expected_digest = P::WorldSaveService::region_simulation_digest(
        {}, key, P::RegionDigestInputs{{chunks[0]}, {}, true});
    EXPECT_NE(expected_digest,
              P::WorldSaveService::region_simulation_digest(moved.GetWorldSaveDir(), key, {}));
    EXPECT_NE(expected_digest, P::WorldSaveService::region_simulation_digest({}, key, {}));

    auto* world = moved.GetWorldSystem();
    // Four render frames reach the activation pass. Water remains attached:
    // render frames cannot mutate authoritative water.
    world->debug_set_streaming_radius_cap(1);
    chunks[1]->has_collision.store(true);
    const auto move_camera = [&](const Vec3& camera) {
        for (int frame = 0; frame < 4; ++frame) {
            world->update(moved.GetRegistry(), camera, nullptr);
            world->wait_for_streaming_jobs();
        }
    };
    move_camera(moved.GetMetadata().spawnPoint);
    ASSERT_EQ(world->find_streamed_chunk(coords), nullptr);
    EXPECT_EQ(world->simulation_chunk_count(), 0u);
    EXPECT_EQ(P::WorldSaveService::region_simulation_digest(moved.GetWorldSaveDir(), key, {}),
              expected_digest);
    EXPECT_FALSE(chunks[1]->has_collision.load());
    ASSERT_EQ(resident.GetWorldSystem()->find_streamed_chunk(coords), chunks[0]);

    const auto dt = resident.GetSimulationClock().fixed_dt();
    for (int tick = 1; tick <= 3; ++tick) {
        for (auto* session : {&resident, &moved}) {
            session->RecordActiveRegionWork({key, 1, 0, 0, 0});
            ASSERT_EQ(session->TickSimulation(dt), 1u);
            ASSERT_FALSE(session->GetRegionSchedulerFault());
        }
        EXPECT_EQ(resident.GetActiveRegionLedger().canonical_bytes(),
                  moved.GetActiveRegionLedger().canonical_bytes());
        EXPECT_EQ(resident.FoldClockIntoEcologyHash(
                      luminumbra::ai::ComputeEcologySubHash(resident.GetRegistry())),
                  moved.FoldClockIntoEcologyHash(
                      luminumbra::ai::ComputeEcologySubHash(moved.GetRegistry())));
    }
    ASSERT_EQ(moved.GetActiveRegionLedger().records().at(key).state, RegionState::Frozen);
    EXPECT_EQ(moved.GetActiveRegionLedger().records().at(key).frozen_digest, expected_digest);
    EXPECT_EQ(resident.GetActiveRegionLedger().records().at(key).frozen_digest, expected_digest);
    ASSERT_TRUE(moved.SaveWorldState());
    EXPECT_FALSE(chunks[1]->is_voxel_data_dirty());
    EXPECT_EQ(P::WorldSaveService::region_simulation_digest(moved.GetWorldSaveDir(), key, {}),
              expected_digest);
    ASSERT_TRUE(loaded.LoadWorld(moved.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_EQ(loaded.GetActiveRegionLedger().canonical_bytes(),
              moved.GetActiveRegionLedger().canonical_bytes());
    EXPECT_EQ(
        loaded.FoldClockIntoEcologyHash(
            luminumbra::ai::ComputeEcologySubHash(loaded.GetRegistry())),
        moved.FoldClockIntoEcologyHash(luminumbra::ai::ComputeEcologySubHash(moved.GetRegistry())));

    // Both synchronous surface loading and ordinary streaming restore the
    // durable authority rather than generating over edited data.
    world->EnsureSurfaceReadyNear(edit_anchor, moved.GetPhysicsSystem(), 0, 0);
    ASSERT_NE(world->find_streamed_chunk(coords), nullptr);
    EXPECT_NE(world->find_streamed_chunk(coords), chunks[1]);
    EXPECT_EQ(world->simulation_chunk_count(), 0u);
    move_camera(moved.GetMetadata().spawnPoint);
    ASSERT_EQ(world->find_streamed_chunk(coords), nullptr);
    move_camera(edit_anchor);
    ASSERT_NE(world->find_streamed_chunk(coords), nullptr);
    EXPECT_NE(world->find_streamed_chunk(coords), chunks[1]);
    EXPECT_NE(world->find_streamed_chunk(coords)->get_state(), ChunkState::Loading);
    EXPECT_EQ(world->simulation_chunk_count(), 0u);
    EXPECT_EQ(P::WorldSaveService::region_simulation_digest(
                  {},
                  key,
                  P::RegionDigestInputs{world->snapshot_region_authority(key.x, key.z), {}, true}),
              expected_digest);
    world->clear_world(nullptr);
    EXPECT_TRUE(world->snapshot_region_authority(key.x, key.z).empty());
    EXPECT_EQ(world->simulation_chunk_count(), 0u);
}

namespace {
std::shared_ptr<Luminumbra::Chunk> FlowingRegionChunk(const Luminumbra::IVec3& coords) {
    auto chunk = std::make_shared<Luminumbra::Chunk>(coords);
    chunk->set_state(Luminumbra::ChunkState::Idle);
    chunk->sdf_data.assign(std::size_t{17} * 17 * 17, 1.0f);
    chunk->heightmap_data.assign(std::size_t{17} * 17, 0.0f);
    chunk->material_data.assign(std::size_t{17} * 17 * 17, 2);
    constexpr std::size_t cells = std::size_t{16} * 16;
    chunk->has_water_sim.store(true);
    chunk->current_water_resolution.store(16);
    chunk->water_depth_mm.assign(cells, 1000);
    chunk->water_depth_mm[15] = 4000;
    chunk->water_bed_mm.assign(cells, 0);
    chunk->water_edge_flux.assign(2 * cells, 7);
    chunk->water_level_data.assign(cells, 1.0f);
    chunk->water_flow_data.assign(cells, Luminumbra::Vec2(0.0f));
    chunk->water_sim_terrain_height.assign(cells, 0.0f);
    chunk->water_rest_level.assign(cells, 0.0f);
    chunk->is_water_sleeping.store(false);
    chunk->max_water_delta_last_tick = 0.25f;
    chunk->ticks_below_threshold = 2;
    chunk->mark_voxel_data_dirty();
    return chunk;
}
} // namespace

TEST(GameSessionHeadlessWorldTest, WaterOnlyColumnOwnersRenderAndRemeshAboveDeepSeabeds) {
    using Luminumbra::IVec3;
    using Luminumbra::Vec3;
    const HeadlessRoot root;
    const auto preset_path = root.path() / "worlds/atlas/presets/default.json";
    auto preset = nlohmann::json::parse(ReadClockTestFile(preset_path));
    preset["generation_params"]["terrain"]["height_offset"] = -256.0;
    preset["generation_params"]["terrain"]["hydro"]["enabled"] = false;
    std::ofstream(preset_path) << preset.dump();
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateTransientWorld("Water rendering", "1337", "default"));
    auto* world = session.GetWorldSystem();
    const float height = world->GetTerrainHeightAt(8, 8);
    ASSERT_LT(height, -32.0f);
    ASSERT_TRUE(
        world->EnsureSurfaceReadyNear(Vec3(8, height + 8, 8), session.GetPhysicsSystem(), 1, 0));
    auto owner = world->find_streamed_chunk(IVec3(0, 0, 0));
    ASSERT_NE(owner, nullptr);
    EXPECT_TRUE(owner->mesh_vertices.empty());
    EXPECT_TRUE(owner->mesh_indices.empty());
    ASSERT_FALSE(owner->water_mesh_vertices.empty());
    ASSERT_FALSE(owner->water_mesh_indices.empty());
    auto visible = world->get_renderable_chunks();
    EXPECT_NE(std::find(visible.begin(), visible.end(), owner.get()), visible.end());
    EXPECT_EQ(world->get_runtime_chunk_stats().renderable_chunks, visible.size());
    const auto depths = owner->water_depth_mm;
    world->debug_set_streaming_radius_cap(1);
    bool settled = false;
    for (int frame = 0; frame < 24; ++frame) {
        world->update(session.GetRegistry(), Vec3(8, height + 8, 8), nullptr);
        world->wait_for_streaming_jobs();
        const auto& stats = world->get_last_streaming_budget_stats();
        if (stats.meshing_candidates == 0 && stats.generation_candidates == 0) {
            settled = true;
            break;
        }
    }
    ASSERT_TRUE(settled);
    const auto version = owner->water_mesh_version.load();
    owner->water_mesh_generated.store(false);
    world->update(session.GetRegistry(), Vec3(8, height + 8, 8), nullptr);
    world->wait_for_streaming_jobs();
    EXPECT_GT(owner->water_mesh_version.load(), version);
    EXPECT_GT(owner->current_lod.load(), 0);
    EXPECT_TRUE(owner->water_mesh_generated.load());
    EXPECT_FALSE(owner->water_mesh_indices.empty());
    EXPECT_EQ(owner->water_depth_mm, depths);
    visible = world->get_renderable_chunks();
    EXPECT_NE(std::find(visible.begin(), visible.end(), owner.get()), visible.end());

    // Dry or malformed grids must neither allocate from an unchecked resolution
    // nor retain a stale water mesh that can establish far-world ownership.
    for (const int resolution : {0, -1, std::numeric_limits<int>::max(), 16}) {
        owner->current_water_resolution.store(resolution);
        owner->water_level_data.clear();
        EXPECT_FALSE(world->debug_water_grid_at(8, 8));
        EXPECT_FLOAT_EQ(world->live_water_surface_at(8, 8), height);
        Luminumbra::World::MarchingCubes::GenerateWaterMesh(
            *session.GetWaterSystem(), *world, *owner);
        EXPECT_TRUE(owner->water_mesh_vertices.empty());
        EXPECT_TRUE(owner->water_mesh_indices.empty());
        EXPECT_FALSE(owner->water_mesh_generated.load());
        visible = world->get_renderable_chunks();
        EXPECT_EQ(std::find(visible.begin(), visible.end(), owner.get()), visible.end());
    }
}

TEST(GameSessionHeadlessWorldTest, ColumnWaterSeamsRejectPartialBedsOnEitherSide) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    session.SetWeatherRainEnabled(false);
    session.SetWaterHighResEnabled(true);
    ASSERT_TRUE(session.CreateTransientWorld("Seam validation", "1337", "default"));
    auto* water = session.GetWaterSystem();
    water->SetHydrology(true, 0, 0);
    for (const int partial : {-1, 0, 1}) {
        auto left = FlowingRegionChunk({0, 0, 0});
        auto right = FlowingRegionChunk({1, 0, 0});
        if (partial == 0)
            left->water_bed_mm.pop_back();
        if (partial == 1)
            right->water_bed_mm.pop_back();
        const std::unordered_map<Luminumbra::ChunkID, std::shared_ptr<Luminumbra::Chunk>> chunks{
            {left->get_id(), left}, {right->get_id(), right}};
        water->update(session.GetRegistry(), chunks, true);
        if (partial < 0)
            EXPECT_GT(water->dbg_seam_wet_pairs(), 0);
        else
            EXPECT_EQ(water->dbg_seam_wet_pairs(), 0);
        EXPECT_TRUE(water->dbg_mass_ok());
    }
    auto chunk = FlowingRegionChunk({0, 0, 0});
    const auto depths = chunk->water_depth_mm;
    for (const int invalid : {1, 33, std::numeric_limits<int>::max()}) {
        water->ResizeSimulationGrid(*chunk,
                                    static_cast<Luminumbra::Systems::WaterDetailLevel>(invalid));
        EXPECT_EQ(chunk->current_water_resolution.load(), 16);
        EXPECT_EQ(chunk->water_depth_mm, depths);
    }
}

TEST(GameSessionHeadlessWorldTest, AuthorityRestoreBatchesReuseIndexesAndDiscardOrphanedJobs) {
    using Luminumbra::IVec3;
    using Luminumbra::Vec3;
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Restore batches", "1337", "default"));
    auto* world = session.GetWorldSystem();
    const std::vector coordinates{IVec3(1, 0, 2), IVec3(2, 0, 2)};
    for (const auto& coords : coordinates) {
        auto chunk = FlowingRegionChunk(coords);
        chunk->set_state(Luminumbra::ChunkState::Meshing);
        chunk->pending_lod.store(2);
        chunk->pending_mesh_ready.store(true);
        chunk->pending_mesh_failed.store(true);
        chunk->pending_promotion_ready.store(true);
        chunk->pending_promotion_failed.store(true);
        chunk->pending_generation_ready.store(true);
        ASSERT_TRUE(P::WorldSaveService::persist_chunk_authority(session.GetWorldSaveDir(), chunk));
    }
    const auto expect_normalized = [&](const IVec3& coords) {
        const auto chunk = world->find_streamed_chunk(coords);
        ASSERT_NE(chunk, nullptr);
        EXPECT_EQ(chunk->pending_lod.load(), -1);
        EXPECT_FALSE(chunk->pending_mesh_ready.load());
        EXPECT_FALSE(chunk->pending_mesh_failed.load());
        EXPECT_FALSE(chunk->pending_promotion_ready.load());
        EXPECT_FALSE(chunk->pending_promotion_failed.load());
        EXPECT_FALSE(chunk->pending_generation_ready.load());
        EXPECT_TRUE(chunk->pending_mesh_vertices.empty());
        EXPECT_TRUE(chunk->pending_water_mesh_vertices.empty());
        EXPECT_EQ(chunk->water_depth_mm, FlowingRegionChunk(coords)->water_depth_mm);
    };
    auto reads = world->authority_region_index_reads();
    world->prepare_region_authority(session.GetRegistry(), {{0, 0}}, {Vec3(40, 8, 40)}, nullptr);
    EXPECT_EQ(world->authority_region_index_reads() - reads, 1u);
    for (const auto& coords : coordinates)
        ASSERT_NO_FATAL_FAILURE(expect_normalized(coords));

    world->clear_world(nullptr);
    reads = world->authority_region_index_reads();
    world->dispatch_generation_jobs(coordinates);
    world->wait_for_streaming_jobs();
    EXPECT_EQ(world->authority_region_index_reads() - reads, 1u);
    for (const auto& coords : coordinates)
        ASSERT_NO_FATAL_FAILURE(expect_normalized(coords));

    world->clear_world(nullptr);
    reads = world->authority_region_index_reads();
    const float height = world->GetTerrainHeightAt(24, 40);
    ASSERT_TRUE(
        world->EnsureSurfaceReadyNear(Vec3(24, height + 8, 40), session.GetPhysicsSystem(), 0, 0));
    EXPECT_EQ(world->authority_region_index_reads() - reads, 1u);
    ASSERT_NO_FATAL_FAILURE(expect_normalized(coordinates.front()));
    EXPECT_EQ(world->find_streamed_chunk(coordinates.front())->get_state(),
              Luminumbra::ChunkState::Ready);
}

TEST(GameSessionHeadlessWorldTest, IndexedAuthorityLookupRefusesPartialUnknownAndUnreadableData) {
    using Save = P::WorldSaveService;
    const HeadlessRoot root;
    const auto directory = root.path() / "authority-reader";
    const auto chunk = FlowingRegionChunk({1, 0, 2});
    ASSERT_TRUE(Save::persist_chunk_authority(directory, chunk));
    const auto path = Save::region_file_path(directory, 0, 0);
    const auto original = ReadClockTestFile(path);
    Save::AuthorityReader reader;
    ASSERT_TRUE(reader.open(path));
    EXPECT_EQ(reader.next_id(), chunk->get_id());
    EXPECT_EQ(reader.read(Luminumbra::Chunk::calculate_id({3, 0, 2})), nullptr);
    EXPECT_EQ(reader.next_id(), chunk->get_id());
    ASSERT_NE(reader.read(chunk->get_id()), nullptr);
    ASSERT_NE(reader.read_next(), nullptr);
    EXPECT_FALSE(reader.next_id());
    EXPECT_EQ(reader.read_next(), nullptr);
    reader.skip();
    EXPECT_FALSE(reader.next_id());
    reader = {};
    EXPECT_TRUE(reader.open(Save::region_file_path(directory, 4, 4)));
    EXPECT_EQ(reader.read(chunk->get_id()), nullptr);
    EXPECT_FALSE(reader.next_id());
    EXPECT_FALSE(reader.open(path.parent_path()));
    EXPECT_EQ(reader.read_next(), nullptr);
    for (const std::size_t size : {std::size_t{0}, std::size_t{7}, original.size() - 1}) {
        std::ofstream(path, std::ios::binary | std::ios::trunc) << original.substr(0, size);
        EXPECT_FALSE(reader.open(path));
        EXPECT_FALSE(reader.next_id());
        EXPECT_EQ(reader.read(chunk->get_id()), nullptr);
    }
    auto unknown = original;
    unknown[4] = static_cast<char>(0x7f);
    std::ofstream(path, std::ios::binary | std::ios::trunc) << unknown;
    EXPECT_FALSE(reader.open(path));
    auto corrupt_payload = original;
    // One record: eight-byte container header and eighteen-byte record header.
    ASSERT_GT(corrupt_payload.size(), 26u);
    std::fill(corrupt_payload.begin() + 26, corrupt_payload.end(), '\0');
    std::ofstream(path, std::ios::binary | std::ios::trunc) << corrupt_payload;
    ASSERT_TRUE(reader.open(path));
    EXPECT_THROW(reader.read(chunk->get_id()), std::runtime_error);
    EXPECT_EQ(reader.next_id(), chunk->get_id());
    reader = {};
    std::ofstream(path, std::ios::binary | std::ios::trunc) << original;
    ASSERT_TRUE(reader.open(path));
    EXPECT_NE(reader.read_next(), nullptr);
}

TEST(GameSessionHeadlessWorldTest, BoundaryWaterAndFreezeIgnoreCameraFramesAndRestore) {
    using Luminumbra::IVec3;
    using Luminumbra::Vec3;
    using Luminumbra::world::RegionKey;
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession resident, restored, reloaded;
    for (auto* session : {&resident, &restored, &reloaded}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetActiveRegionsEnabled(true);
        session->SetWeatherRainEnabled(false);
        session->SetWaterHighResEnabled(true);
        session->SetRegionSchedulerConfig({0, 1, 1});
    }
    ASSERT_TRUE(resident.CreateWorld("Boundary resident", "1337", "default"));
    ASSERT_TRUE(restored.CreateWorld("Boundary durable", "1337", "default"));
    const std::array keys{RegionKey{0, 0}, RegionKey{1, 0}};
    for (const auto coords : {IVec3(31, 0, 0), IVec3(32, 0, 0)}) {
        auto live = FlowingRegionChunk(coords);
        auto durable = FlowingRegionChunk(coords);
        ASSERT_TRUE(resident.GetWorldSystem()->adopt_streamed_chunk(live));
        ASSERT_TRUE(
            P::WorldSaveService::persist_chunk_authority(restored.GetWorldSaveDir(), durable));
    }
    for (auto* session : {&resident, &restored}) {
        session->SetReplicatedSimulationAnchors({Vec3(504, 8, 8)});
        for (const auto key : keys)
            session->NotifyGroundObjectEdit(Vec3(static_cast<float>(key.x * 512 + 8), 8, 8));
    }
    auto* camera_world = restored.GetWorldSystem();
    camera_world->debug_set_streaming_radius_cap(1);
    const auto camera_frames = [&] {
        for (int frame = 0; frame < 4; ++frame) {
            camera_world->update(restored.GetRegistry(), Vec3(-4096, 8, -4096), nullptr);
            camera_world->wait_for_streaming_jobs();
        }
    };
    camera_frames();
    // Rehydration must replace unrelated render-cache contents at the same ids,
    // including their collider bookkeeping, before any authoritative water tick.
    for (const auto coords : {IVec3(31, 0, 0), IVec3(32, 0, 0)}) {
        auto cache = std::make_shared<Luminumbra::Chunk>(coords);
        cache->set_state(Luminumbra::ChunkState::Idle);
        cache->heightmap_data.assign(std::size_t{17} * 17, 999.0f);
        cache->has_collision.store(true);
        ASSERT_TRUE(camera_world->adopt_streamed_chunk(cache));
    }
    const auto dt = resident.GetSimulationClock().fixed_dt();
    for (int tick = 0; tick < 8; ++tick) {
        ASSERT_EQ(resident.TickSimulation(dt), 1u);
        ASSERT_EQ(restored.TickSimulation(dt), 1u);
        ASSERT_FALSE(resident.GetRegionSchedulerFault());
        ASSERT_FALSE(restored.GetRegionSchedulerFault());
        if (tick == 0) {
            EXPECT_GT(resident.GetWorldSystem()->debug_water_seam_wet_pairs(), 0);
        }
        EXPECT_TRUE(camera_world->find_streamed_chunk(IVec3(31, 0, 0))->has_collision.load());
        const auto restored_ground =
            restored.GetPhysicsSystem()->audio_raycast(Vec3(504, 2, 8), Vec3(504, -2, 8));
        ASSERT_TRUE(restored_ground.hit);
        EXPECT_NEAR(restored_ground.hit_point.y, 0.0f, 0.01f);
        const auto before_frames = P::WorldSaveService::region_simulation_digest(
            {},
            keys[0],
            P::RegionDigestInputs{camera_world->snapshot_region_authority(0, 0), {}, true});
        camera_frames();
        EXPECT_EQ(
            before_frames,
            P::WorldSaveService::region_simulation_digest(
                {},
                keys[0],
                P::RegionDigestInputs{camera_world->snapshot_region_authority(0, 0), {}, true}));
        for (const auto key : keys) {
            EXPECT_EQ(P::WorldSaveService::region_simulation_digest(
                          {},
                          key,
                          P::RegionDigestInputs{
                              resident.GetWorldSystem()->snapshot_region_authority(key.x, key.z),
                              {},
                              true}),
                      P::WorldSaveService::region_simulation_digest(
                          restored.GetWorldSaveDir(),
                          key,
                          P::RegionDigestInputs{
                              camera_world->snapshot_region_authority(key.x, key.z), {}, true}));
        }
        const auto live_oracle = resident.GetWorldSystem()->compute_chunk_hashes();
        const auto durable_oracle = camera_world->compute_chunk_hashes();
        EXPECT_EQ(live_oracle.world, durable_oracle.world);
        EXPECT_EQ(live_oracle.sub.water, durable_oracle.sub.water);
        EXPECT_EQ(resident.GetActiveRegionLedger().canonical_bytes(),
                  restored.GetActiveRegionLedger().canonical_bytes());
    }

    ASSERT_NE(resident.GetWorldSystem()->find_streamed_chunk(IVec3(31, 0, 0))->water_depth_mm,
              FlowingRegionChunk(IVec3(31, 0, 0))->water_depth_mm);
    ASSERT_TRUE(restored.SaveWorldState());
    ASSERT_TRUE(reloaded.LoadWorld(restored.GetMetadata().worldId));
    reloaded.SetReplicatedSimulationAnchors({Vec3(504, 8, 8)});
    for (auto* session : {&resident, &restored, &reloaded})
        ASSERT_EQ(session->TickSimulation(dt), 1u);
    EXPECT_TRUE(
        reloaded.GetWorldSystem()->find_streamed_chunk(IVec3(31, 0, 0))->is_voxel_data_dirty());
    for (const auto key : keys) {
        EXPECT_EQ(
            P::WorldSaveService::region_simulation_digest(
                {},
                key,
                P::RegionDigestInputs{
                    resident.GetWorldSystem()->snapshot_region_authority(key.x, key.z), {}, true}),
            P::WorldSaveService::region_simulation_digest(
                reloaded.GetWorldSaveDir(),
                key,
                P::RegionDigestInputs{
                    reloaded.GetWorldSystem()->snapshot_region_authority(key.x, key.z), {}, true}));
    }
    for (auto* session : {&resident, &restored, &reloaded}) {
        session->SetReplicatedSimulationAnchors({});
        // Keep unrelated activation-disc records above the two boundary fixtures
        // in scheduler priority; only one budget transition is allowed per tick.
        for (const auto& [key, record] : session->GetActiveRegionLedger().records()) {
            (void)record;
            if (key != keys[0] && key != keys[1])
                session->PinActiveRegion(key, true);
        }
    }
    for (int tick = 0; tick < 8; ++tick) {
        for (auto* session : {&resident, &restored, &reloaded}) {
            for (const auto key : keys)
                session->RecordActiveRegionWork({key, 1, 0, 0, 0});
            ASSERT_EQ(session->TickSimulation(dt), 1u);
            ASSERT_FALSE(session->GetRegionSchedulerFault());
        }
        camera_frames();
        for (const auto key : keys)
            EXPECT_TRUE(camera_world->snapshot_region_authority(key.x, key.z).empty());
        for (auto* peer : {&restored, &reloaded}) {
            EXPECT_EQ(resident.GetWorldSystem()->compute_chunk_hashes().world,
                      peer->GetWorldSystem()->compute_chunk_hashes().world);
            EXPECT_EQ(resident.GetWorldSystem()->compute_chunk_hashes().sub.water,
                      peer->GetWorldSystem()->compute_chunk_hashes().sub.water);
            EXPECT_EQ(resident.GetActiveRegionLedger().canonical_bytes(),
                      peer->GetActiveRegionLedger().canonical_bytes());
            EXPECT_EQ(resident.FoldClockIntoEcologyHash(
                          luminumbra::ai::ComputeEcologySubHash(resident.GetRegistry())),
                      peer->FoldClockIntoEcologyHash(
                          luminumbra::ai::ComputeEcologySubHash(peer->GetRegistry())));
        }
    }
    for (const auto key : keys) {
        const auto& record = resident.GetActiveRegionLedger().records().at(key);
        EXPECT_EQ(record.state, Luminumbra::world::RegionState::Frozen);
        EXPECT_NE(record.frozen_digest, 0u);
    }
}

TEST(GameSessionHeadlessWorldTest, BoundedSnapshotLoadValidatesRecordsBeyondResidentPrefix) {
    const HeadlessRoot root;
    const auto directory = root.path() / "bounded-save";
    auto first = FlowingRegionChunk(Luminumbra::IVec3(0, 0, 0));
    auto second = FlowingRegionChunk(Luminumbra::IVec3(32, 0, 0));
    ASSERT_TRUE(P::WorldSaveService::persist_chunk_authority(directory, first));
    ASSERT_TRUE(P::WorldSaveService::persist_chunk_authority(directory, second));
    const P::WorldSaveService service;
    Luminumbra::WorldStreamingState state;
    std::vector<std::string> errors;
    ASSERT_TRUE(service.load_world(state, directory, errors, {}, 1, true));
    EXPECT_EQ(state.size(), 1u);
    EXPECT_TRUE(errors.empty());
    // A bad record beyond the retained prefix still refuses the complete load.
    second->sdf_data.resize(3);
    second->mark_voxel_data_dirty();
    ASSERT_TRUE(P::WorldSaveService::persist_chunk_authority(directory, second));
    EXPECT_FALSE(service.load_world(state, directory, errors, {}, 1, true));
    EXPECT_TRUE(state.empty());
    ASSERT_FALSE(errors.empty());
    EXPECT_NE(errors.front().find("SDF lattice"), std::string::npos);
}

TEST(GameSessionHeadlessWorldTest, RenderingCannotRegenerateCoarseEditedAuthority) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    for (const bool water_only : {false, true}) {
        SCOPED_TRACE(water_only);
        GameSession session;
        session.SetRootPath(root.root_string());
        session.SetJobSystem(&jobs);
        session.SetActiveRegionsEnabled(true);
        ASSERT_TRUE(session.CreateTransientWorld("Coarse authority", "1337", "default"));
        auto* world = session.GetWorldSystem();
        const float x = 520.0f, z = 520.0f;
        const float height = world->GetTerrainHeightAt(x, z);
        const Luminumbra::IVec3 coords(32, static_cast<int>(std::floor(height / 16.0f)), 32);
        auto chunk = FlowingRegionChunk(coords);
        chunk->sdf_data.clear();
        if (water_only) {
            chunk->mark_sdf_generated_current_params();
            chunk->mark_water_data_dirty();
        }
        ASSERT_TRUE(world->adopt_streamed_chunk(chunk));
        const auto before = P::WorldSaveService::region_simulation_digest(
            {}, {1, 1}, P::RegionDigestInputs{{chunk}, {}, true});
        world->EnsureSurfaceReadyNear(
            Luminumbra::Vec3(x, height + 8, z), session.GetPhysicsSystem(), 0, 0);
        world->wait_for_streaming_jobs();
        world->debug_set_streaming_radius_cap(1);
        world->update(session.GetRegistry(), Luminumbra::Vec3(x, height + 8, z), nullptr);
        world->wait_for_streaming_jobs();
        EXPECT_TRUE(chunk->sdf_data.empty());
        EXPECT_EQ(P::WorldSaveService::region_simulation_digest(
                      {}, {1, 1}, P::RegionDigestInputs{{chunk}, {}, true}),
                  before);
    }
}

TEST(GameSessionHeadlessWorldTest, ExplicitWaterSourcesOwnChunksWithoutRendering) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    session.SetWeatherRainEnabled(false);
    ASSERT_TRUE(session.CreateTransientWorld("Source authority", "1337", "default"));
    const Luminumbra::Vec3 position(520, 40, 24);
    session.SetReplicatedSimulationAnchors({position});
    const auto source = session.GetRegistry().create();
    session.GetRegistry().emplace<Luminumbra::Components::TransformComponent>(source).position =
        position;
    session.GetRegistry()
        .emplace<Luminumbra::Components::FlowingWaterSourceComponent>(source)
        .flow_rate = 40.0f;
    for (const float height : {std::numeric_limits<float>::quiet_NaN(), -100000.0f, 100000.0f}) {
        const auto invalid_source = session.GetRegistry().create();
        session.GetRegistry()
            .emplace<Luminumbra::Components::TransformComponent>(invalid_source)
            .position = Luminumbra::Vec3(position.x, height, position.z);
        session.GetRegistry()
            .emplace<Luminumbra::Components::FlowingWaterSourceComponent>(invalid_source)
            .flow_rate = 40.0f;
    }
    // Irrigation is an independent component with a different layout. The two
    // emitters must coexist without aliasing entt storage across systems.
    session.GetRegistry().emplace<Luminumbra::Components::WaterSourceComponent>(source).output =
        123;
    ASSERT_TRUE(session.GetWorldSystem()->snapshot_streamed_chunks().empty());
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    auto chunk = session.GetWorldSystem()->find_streamed_chunk(Luminumbra::IVec3(32, 0, 1));
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(chunk->has_water_sim.load());
    EXPECT_EQ(
        session.GetRegistry().get<Luminumbra::Components::WaterSourceComponent>(source).output,
        123);
    EXPECT_EQ(session.GetRegistry()
                  .get<Luminumbra::Components::FlowingWaterSourceComponent>(source)
                  .flow_rate,
              40.0f);
    std::set<Luminumbra::ChunkID> expected_chunks;
    const auto centre = Luminumbra::Systems::SHIELD_WorldSystem::world_to_chunk_coords(position);
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            const float x = static_cast<float>(centre.x + dx) * 16 + 8;
            const float z = static_cast<float>(centre.z + dz) * 16 + 8;
            const auto* world = session.GetWorldSystem();
            const float terrain = world->GetTerrainHeightAt(x, z);
            for (const float height : {0.0f, terrain}) {
                const auto coords = Luminumbra::Systems::SHIELD_WorldSystem::world_to_chunk_coords(
                    Luminumbra::Vec3(x, height, z));
                expected_chunks.insert(Luminumbra::Chunk::calculate_id(coords));
                ASSERT_NE(world->find_streamed_chunk(coords), nullptr);
                EXPECT_EQ(world->find_streamed_chunk(coords)->has_water_sim.load(), coords.y == 0);
            }
        }
    }
    expected_chunks.insert(chunk->get_id());
    EXPECT_EQ(session.GetWorldSystem()->simulation_chunk_count(), expected_chunks.size());
    EXPECT_GT(session.GetWorldSystem()->debug_water_source_injected_mm(), 0);
}

TEST(GameSessionHeadlessWorldTest, WaterInitializationUsesIdsAcrossDifferentInsertionOrders) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession forward, reverse;
    for (auto* session : {&forward, &reverse}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetActiveRegionsEnabled(true);
        session->SetWeatherRainEnabled(false);
        session->SetWaterHighResEnabled(false);
        ASSERT_TRUE(session->CreateTransientWorld("Water admission", "1337", "default"));
        session->SetReplicatedSimulationAnchors({Luminumbra::Vec3(504, 8, 8)});
    }
    for (int index = 0; index < 10; ++index) {
        for (auto* session : {&forward, &reverse}) {
            const int x = session == &forward ? 27 + index : 36 - index;
            auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(x, 0, 0));
            chunk->sdf_data.assign(std::size_t{17} * 17 * 17, 1.0f);
            chunk->heightmap_data.assign(std::size_t{17} * 17, 0.0f);
            chunk->current_lod.store(session == &forward ? 0 : 2);
            chunk->set_state(Luminumbra::ChunkState::Idle);
            chunk->mark_voxel_data_dirty();
            ASSERT_TRUE(session->GetWorldSystem()->adopt_streamed_chunk(chunk));
        }
    }
    for (int tick = 0; tick < 3; ++tick) {
        ASSERT_EQ(forward.TickSimulation(forward.GetSimulationClock().fixed_dt()), 1u);
        ASSERT_EQ(reverse.TickSimulation(reverse.GetSimulationClock().fixed_dt()), 1u);
        for (const auto key :
             {Luminumbra::world::RegionKey{0, 0}, Luminumbra::world::RegionKey{1, 0}}) {
            EXPECT_EQ(P::WorldSaveService::region_simulation_digest(
                          {},
                          key,
                          P::RegionDigestInputs{
                              forward.GetWorldSystem()->snapshot_region_authority(key.x, key.z),
                              {},
                              true}),
                      P::WorldSaveService::region_simulation_digest(
                          {},
                          key,
                          P::RegionDigestInputs{
                              reverse.GetWorldSystem()->snapshot_region_authority(key.x, key.z),
                              {},
                              true}));
        }
    }
}

TEST(GameSessionHeadlessWorldTest,
     SuccessiveEditedRegionsReleaseChunksAndRestoreCompleteAuthority) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Bounded authority", "1337", "default"));
    session.SetReplicatedSimulationAnchors({});
    auto* world = session.GetWorldSystem();
    world->debug_set_streaming_radius_cap(1);
    const auto evict = [&] {
        for (int frame = 0; frame < 4; ++frame) {
            world->update(session.GetRegistry(), session.GetMetadata().spawnPoint, nullptr);
            world->wait_for_streaming_jobs();
        }
    };
    for (int region = 0; region < 12; ++region) {
        const Luminumbra::world::RegionKey key{10 + region * 4, -40};
        const Luminumbra::IVec3 coords(key.x * 32 + 1, 0, key.z * 32 + 1);
        auto chunk = FlowingRegionChunk(coords);
        const auto expected = P::WorldSaveService::region_simulation_digest(
            {}, key, P::RegionDigestInputs{{chunk}, {}, true});
        std::weak_ptr<Luminumbra::Chunk> ownership = chunk;
        ASSERT_TRUE(world->adopt_streamed_chunk(chunk));
        chunk.reset();
        evict();
        ASSERT_FALSE(world->authority_fault());
        EXPECT_TRUE(ownership.expired()) << region;
        EXPECT_EQ(world->simulation_chunk_count(), 0u);
        EXPECT_TRUE(world->snapshot_region_authority(key.x, key.z).empty());
        EXPECT_LE(world->snapshot_streamed_chunks().size(), 8192u);
        EXPECT_EQ(P::WorldSaveService::region_simulation_digest(session.GetWorldSaveDir(), key, {}),
                  expected);
        world->dispatch_generation_jobs(std::vector<Luminumbra::IVec3>{coords});
        world->wait_for_streaming_jobs();
        chunk = world->find_streamed_chunk(coords);
        ASSERT_NE(chunk, nullptr);
        EXPECT_EQ(P::WorldSaveService::region_simulation_digest(
                      {}, key, P::RegionDigestInputs{{chunk}, {}, true}),
                  expected);
        EXPECT_FALSE(chunk->is_voxel_data_dirty());
        ownership = chunk;
        chunk.reset();
        evict();
        EXPECT_TRUE(ownership.expired()) << region;
    }
    const auto alternate = root.path() / "bounded-snapshot";
    const auto canonical_authority = world->authority_directory();
    ASSERT_TRUE(session.SaveWorldStateTo(alternate));
    EXPECT_EQ(world->authority_directory(), canonical_authority);
    for (int region = 0; region < 12; ++region) {
        const Luminumbra::world::RegionKey key{10 + region * 4, -40};
        EXPECT_EQ(
            P::WorldSaveService::region_simulation_digest(alternate, key, {}),
            P::WorldSaveService::region_simulation_digest(session.GetWorldSaveDir(), key, {}));
    }
}

TEST(GameSessionHeadlessWorldTest, EvictionPersistenceFailureRetainsAuthorityAndStopsSession) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Failed authority eviction", "1337", "default"));
    session.SetReplicatedSimulationAnchors({});
    const Luminumbra::world::RegionKey key{-40, -40};
    const Luminumbra::IVec3 coords(-1279, 0, -1279);
    auto chunk = FlowingRegionChunk(coords);
    chunk->mark_sdf_generated_current_params();
    chunk->mark_water_data_dirty();
    ASSERT_TRUE(P::WorldSaveService::persist_chunk_authority(session.GetWorldSaveDir(), chunk));
    const auto old_digest =
        P::WorldSaveService::region_simulation_digest(session.GetWorldSaveDir(), key, {});
    chunk->water_depth_mm[0] += 123;
    chunk->mark_water_data_dirty();
    auto* world = session.GetWorldSystem();
    ASSERT_TRUE(world->adopt_streamed_chunk(chunk));
    world->debug_set_streaming_radius_cap(1);
    P::WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    for (int frame = 0; frame < 4; ++frame) {
        world->update(session.GetRegistry(), session.GetMetadata().spawnPoint, nullptr);
        world->wait_for_streaming_jobs();
    }
    P::WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
    ASSERT_TRUE(world->authority_fault());
    EXPECT_EQ(world->find_streamed_chunk(coords), chunk);
    EXPECT_TRUE(chunk->is_voxel_data_dirty());
    EXPECT_EQ(P::WorldSaveService::region_simulation_digest(session.GetWorldSaveDir(), key, {}),
              old_digest);
    EXPECT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 0u);
    ASSERT_TRUE(session.GetRegionSchedulerFault());
    EXPECT_EQ(world->find_streamed_chunk(coords), chunk);
    EXPECT_NE(P::WorldSaveService::region_simulation_digest(
                  {}, key, P::RegionDigestInputs{{chunk}, {}, true}),
              old_digest);
}

TEST(GameSessionHeadlessWorldTest, SnapshotDestinationCannotImportDivergentAuthority) {
    using Luminumbra::IVec3;
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    // Cover a sibling in the same LMR1 file, a destination-only region, and
    // a source with no durable chunks at all. Refusal must precede any write.
    for (const int variant : {0, 1, 2}) {
        SCOPED_TRACE(variant);
        GameSession session;
        session.SetRootPath(root.root_string());
        session.SetJobSystem(&jobs);
        session.SetActiveRegionsEnabled(true);
        ASSERT_TRUE(session.CreateWorld("Snapshot isolation", "1337", "default"));
        session.SetReplicatedSimulationAnchors({});
        auto* world = session.GetWorldSystem();
        if (variant != 2) {
            auto chunk = FlowingRegionChunk(IVec3(0, 0, 0));
            ASSERT_TRUE(world->adopt_streamed_chunk(chunk));
        }
        ASSERT_TRUE(session.SaveWorldState());
        const auto source = world->authority_directory();
        const auto destination = root.path() / ("divergent-snapshot-" + std::to_string(variant));
        fs::copy(source, destination, fs::copy_options::recursive);
        const IVec3 extra_coords(variant == 1 ? 32 : 1, 0, 0);
        auto extra = FlowingRegionChunk(extra_coords);
        extra->heightmap_data.assign(std::size_t{17} * 17, 13.0f);
        ASSERT_TRUE(P::WorldSaveService::persist_chunk_authority(destination, extra));
        ASSERT_TRUE(P::WorldSaveService::validate_save(destination));
        ASSERT_TRUE(session.SavedPresetCompatible(destination));
        const auto before_disk = SavedPresetTestDiskBytes(root.path());
        const auto before_hash = world->compute_chunk_hashes();
        EXPECT_FALSE(session.SaveWorldStateTo(destination));
        EXPECT_FALSE(world->relocate_authority_directory(destination));
        EXPECT_EQ(world->authority_directory(), source);
        EXPECT_EQ(world->compute_chunk_hashes().world, before_hash.world);
        EXPECT_EQ(world->compute_chunk_hashes().sub.water, before_hash.sub.water);
        EXPECT_EQ(SavedPresetTestDiskBytes(root.path()), before_disk);
        // A later visit must generate this chunk, never resurrect the other save's edit.
        world->dispatch_generation_jobs(std::vector<IVec3>{extra_coords});
        world->wait_for_streaming_jobs();
        const auto visited = world->find_streamed_chunk(extra_coords);
        ASSERT_NE(visited, nullptr);
        EXPECT_NE(visited->sdf_provenance(), Luminumbra::ChunkSdfProvenance::LoadedOrEdited);
    }
}

TEST(GameSessionHeadlessWorldTest, RepeatedExportIncludesUnsavedLiveAuthorityAndKeepsExactBytes) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    for (const bool water : {false, true}) {
        for (const bool durable_sibling : {false, true}) {
            SCOPED_TRACE(water);
            SCOPED_TRACE(durable_sibling);
            GameSession session;
            session.SetRootPath(root.root_string());
            session.SetJobSystem(&jobs);
            session.SetActiveRegionsEnabled(true);
            ASSERT_TRUE(session.CreateWorld("Repeated export", "1337", "default"));
            session.SetReplicatedSimulationAnchors({});
            auto* world = session.GetWorldSystem();
            if (durable_sibling) {
                ASSERT_TRUE(world->adopt_streamed_chunk(FlowingRegionChunk({0, 0, 0})));
                ASSERT_TRUE(session.SaveWorldState());
            }
            auto live = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(32, 0, 0));
            if (water) {
                live = FlowingRegionChunk({32, 0, 0});
                live->mark_sdf_generated_current_params();
                live->mark_water_data_dirty();
            } else {
                live->heightmap_data = {12.0f};
                live->mark_voxel_data_dirty();
            }
            ASSERT_TRUE(world->adopt_streamed_chunk(live));
            const auto source = world->authority_directory();
            const auto canonical_bytes = SavedPresetTestDiskBytes(source);
            const auto destination = root.path() / ("repeat-export-" + std::to_string(water) + "-" +
                                                    std::to_string(durable_sibling));
            ASSERT_TRUE(session.SaveWorldStateTo(destination));
            const auto first = SavedPresetTestDiskBytes(destination);
            ASSERT_TRUE(session.SaveWorldStateTo(destination));
            EXPECT_EQ(SavedPresetTestDiskBytes(destination), first);
            EXPECT_EQ(SavedPresetTestDiskBytes(source), canonical_bytes);
            EXPECT_EQ(world->authority_directory(), source);
            EXPECT_TRUE(live->is_voxel_data_dirty());
            EXPECT_EQ(P::WorldSaveService::authority_hashes(destination, {}).world,
                      P::WorldSaveService::authority_hashes(source, {live}).world);
            // A matching live id cannot authorize a corrupt destination payload.
            const auto region = P::WorldSaveService::region_file_path(destination, 1, 0);
            std::vector<P::WorldSaveService::ContainerRecord> records;
            ASSERT_TRUE(P::WorldSaveService::read_container_records(region, records));
            ASSERT_EQ(records.size(), 1u);
            records.front().payload = "partial";
            ASSERT_TRUE(P::WorldSaveService::upsert_container_records(region, records));
            const auto corrupt = SavedPresetTestDiskBytes(destination);
            EXPECT_FALSE(session.SaveWorldStateTo(destination));
            EXPECT_EQ(SavedPresetTestDiskBytes(destination), corrupt);
            EXPECT_EQ(SavedPresetTestDiskBytes(source), canonical_bytes);
            EXPECT_TRUE(live->is_voxel_data_dirty());
        }
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
    ASSERT_TRUE(fs::remove(path));
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
// limit on ticks k-1 and k: Active->Reduced at k-1, then Reduced->Frozen at k,
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
        if (event.tick == k || event.tick == k - 1)
            session.RecordActiveRegionWork({kFaultRegion, 5, 0, 0, 0});
    });
    for (std::uint64_t t = 1; t <= k + 1; ++t)
        session.GetSimulationEventBus().publish(t, "work", "");
}

// The corrupt durable file only exists to make the digest provider throw. A
// save validates every region file, so restore a clean miss before saving.
void RemoveFaultRegionFile(const GameSession& session) {
    std::error_code error;
    fs::remove(P::WorldSaveService::region_file_path(
                   session.GetWorldSaveDir(), kFaultRegion.x, kFaultRegion.z),
               error);
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
    ASSERT_EQ(session.TickSimulation(0.25 * dt), 0u);
    for (std::uint64_t t = 1; t < start; ++t) {
        ASSERT_EQ(session.TickSimulation(dt), 1u);
    }
    ASSERT_FALSE(session.GetRegionSchedulerFault().has_value());
    // Half a tick of slack keeps the clock from rounding the frame down. A fault
    // on the first tick of the frame stops the second tick from running.
    auto completed_clock = session.GetSimulationClock();
    for (std::uint64_t tick = start; tick <= k; ++tick)
        completed_clock.advance(dt);
    ASSERT_EQ(session.TickSimulation(2.5 * dt), k - start + 1);
    const auto fault = session.GetRegionSchedulerFault();
    ASSERT_TRUE(fault.has_value());
    if (fault.has_value()) {
        const auto& snapshot = fault.value();
        EXPECT_EQ(snapshot.tick, k);
        EXPECT_FALSE(snapshot.message.empty());
    }
    EXPECT_EQ(session.GetSimulationTickCount(), k);
    EXPECT_EQ(session.GetSimulationClock().tick_count(), k);
    EXPECT_EQ(session.GetSimulationClock().accumulator(), completed_clock.accumulator());
    EXPECT_EQ(session.GetSimulationClock().dropped_time_seconds(),
              completed_clock.dropped_time_seconds());
    EXPECT_EQ(session.GetSimulationClock().dropped_frame_count(),
              completed_clock.dropped_frame_count());
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
    EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(session));
}
void RunAdmissionFaultCase(const HeadlessRoot& root,
                           JobSystem& jobs,
                           bool prepare_before_frame,
                           std::uint64_t completed) {
    SCOPED_TRACE(completed);
    SCOPED_TRACE(prepare_before_frame);
    GameSession session, loaded;
    for (auto* current : {&session, &loaded}) {
        current->SetRootPath(root.root_string());
        current->SetJobSystem(&jobs);
        current->SetActiveRegionsEnabled(true);
    }
    ASSERT_TRUE(session.CreateWorld("Admission failure", "1337", "default"));
    session.SetReplicatedSimulationAnchors({});
    const auto dt = session.GetSimulationClock().fixed_dt();
    ASSERT_EQ(session.TickSimulation(0.25 * dt), 0u);
    if (completed != 0) {
        ASSERT_EQ(session.TickSimulation(8.0 * dt), completed);
    }
    const auto before_clock = session.GetSimulationClock();
    const auto before_hash = SessionWorldHash(session);
    const auto before_ledger = session.GetActiveRegionLedger().canonical_bytes();
    const auto region_file = P::WorldSaveService::region_file_path(
        session.GetWorldSaveDir(), kFaultRegion.x, kFaultRegion.z);
    fs::create_directories(region_file.parent_path());
    {
        std::ofstream output(region_file, std::ios::binary);
        output << "not a region file";
        ASSERT_TRUE(output.good());
    }
    session.SetReplicatedSimulationAnchors(
        {Luminumbra::Vec3(kFaultRegion.x * 512 + 8, 8, kFaultRegion.z * 512 + 8)});
    int events_run = 0;
    session.GetSimulationEventBus().subscribe([&](const auto&) { ++events_run; });
    session.GetSimulationEventBus().publish(completed + 1, "must not run", "");
    if (prepare_before_frame)
        session.PrepareSimulationCollisions();
    EXPECT_EQ(session.TickSimulation(2.5 * dt), 0u);
    const auto& fault = session.GetRegionSchedulerFault();
    ASSERT_TRUE(fault.has_value());
    if (fault.has_value()) {
        EXPECT_EQ(fault->tick, completed);
    }
    EXPECT_EQ(session.GetSimulationTickCount(), completed);
    EXPECT_EQ(session.GetSimulationClock().tick_count(), completed);
    EXPECT_EQ(session.GetSimulationClock().accumulator(), before_clock.accumulator());
    EXPECT_EQ(session.GetSimulationClock().dropped_time_seconds(),
              before_clock.dropped_time_seconds());
    EXPECT_EQ(session.GetSimulationClock().dropped_frame_count(),
              before_clock.dropped_frame_count());
    EXPECT_EQ(session.GetActiveRegionLedger().canonical_bytes(), before_ledger);
    EXPECT_EQ(events_run, 0);
    EXPECT_TRUE(session.IsSimulationTickBoundary());
    EXPECT_EQ(session.TickSimulation(dt), 0u);
    ASSERT_TRUE(fs::remove(region_file));
    EXPECT_EQ(SessionWorldHash(session), before_hash);
    ASSERT_TRUE(session.SaveWorldState());
    ASSERT_TRUE(loaded.LoadWorld(session.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_EQ(loaded.GetSimulationTickCount(), completed);
    EXPECT_EQ(SessionWorldHash(loaded), before_hash);
}
} // namespace

TEST(GameSessionHeadlessWorldTest, SchedulerFaultStopsTheFrameAndLeavesASavableConsistentSession) {
    for (const auto& [k, start] : {std::pair<std::uint64_t, std::uint64_t>{3, 3},
                                   std::pair<std::uint64_t, std::uint64_t>{3, 2},
                                   std::pair<std::uint64_t, std::uint64_t>{4, 4}}) {
        ASSERT_NO_FATAL_FAILURE(RunSchedulerFaultCase(k, start));
    }
}

TEST(GameSessionHeadlessWorldTest, AdmissionFailureDoesNotCommitAnUnsimulatedTick) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    for (const bool prepare_before_frame : {false, true}) {
        for (const std::uint64_t completed : {0u, 2u}) {
            ASSERT_NO_FATAL_FAILURE(
                RunAdmissionFaultCase(root, jobs, prepare_before_frame, completed));
        }
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
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(64, 0, 64));
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
    auto resident = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(64, 0, 64));
    resident->set_state(Luminumbra::ChunkState::Meshing);
    resident->heightmap_data = {9.0f};
    resident->mark_voxel_data_dirty();
    ASSERT_TRUE(victim.GetWorldSystem()->adopt_streamed_chunk(resident));
    ASSERT_FALSE(victim.GetWorldSystem()->snapshot_streamed_chunks().empty());
    EXPECT_FALSE(victim.LoadWorldStateFrom(save));
    EXPECT_TRUE(victim.GetWorldSystem()->snapshot_streamed_chunks().empty());
    EXPECT_EQ(victim.GetWorldOpenError(), Luminumbra::world::ActiveRegionLedger::kCorruptMessage);
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
    EXPECT_EQ(direct.LegacyRevision3SystemConfigIdentity(), "60ab3c242d54ce89");
    EXPECT_EQ(baseline, "2a0d6f2dc4028dda");
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

TEST(ActiveRegions, IncrementalRegionDigestPreservesCanonicalJsonBytes) {
    namespace P = Luminumbra::Persistence;
    Luminumbra::WorldStreamingState state;
    const auto verify = [&state] {
        const auto snapshot =
            nlohmann::json::parse(P::SerializeWorldStreamingStateSnapshotJson(state));
        auto contents = nlohmann::json::array();
        for (const auto& chunk : snapshot.at("chunks")) {
            nlohmann::json content;
            for (const auto* field : {"chunk_id",
                                      "coords",
                                      "sdf_data",
                                      "heightmap_data",
                                      "material_data",
                                      "water_depth_mm",
                                      "water_bed_mm",
                                      "water_edge_flux",
                                      "has_water_sim",
                                      "current_water_resolution",
                                      "is_water_sleeping",
                                      "max_water_delta_last_tick",
                                      "ticks_below_threshold"})
                content[field] = chunk.at(field);
            contents.push_back(std::move(content));
        }
        const nlohmann::json canonical = {{"hash_scope", "region_content_v2"},
                                          {"chunks", contents}};
        EXPECT_EQ(P::ComputeRegionContentHash(state), P::StableChecksum(canonical.dump(4) + "\n"));
    };
    verify();
    state.insert_chunk(FlowingRegionChunk({32, 0, 0}));
    verify();
    state.insert_chunk(FlowingRegionChunk({31, 0, 0}));
    verify();
}

TEST(GameSessionHeadlessWorldTest, ExplicitReloadRestoresWaterWindowAfterRewind) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    session.SetWaterHighResEnabled(true);
    session.SetWeatherRainEnabled(false);
    ASSERT_TRUE(session.CreateWorld("Water window", "1337", "default"));
    for (int x = 0; x < 24; ++x)
        ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(
            FlowingRegionChunk(Luminumbra::IVec3(x, 0, 0))));
    const Luminumbra::Vec3 anchor(8, 8, 8);
    session.SetReplicatedSimulationAnchors({anchor});
    const auto dt = session.GetSimulationClock().fixed_dt();
    ASSERT_EQ(session.TickSimulation(dt), 1u);
    const auto cursor = session.GetWorldSystem()->GetWaterSimWindowCursor();
    ASSERT_NE(cursor, 0u);
    const auto save = root.path() / "water-window";
    ASSERT_TRUE(session.SaveWorldStateTo(save));
    ASSERT_EQ(session.TickSimulation(dt), 1u);
    const auto expected = P::WorldSaveService::region_simulation_digest(
        {},
        {0, 0},
        P::RegionDigestInputs{session.GetWorldSystem()->snapshot_region_authority(0, 0), {}, true});
    const auto expected_ledger = session.GetActiveRegionLedger().canonical_bytes();
    ASSERT_TRUE(session.LoadWorldStateFrom(save));
    EXPECT_EQ(session.GetWorldSystem()->GetWaterSimWindowCursor(), cursor);
    session.SetReplicatedSimulationAnchors({anchor});
    ASSERT_EQ(session.TickSimulation(dt), 1u);
    EXPECT_EQ(P::WorldSaveService::region_simulation_digest(
                  {},
                  {0, 0},
                  P::RegionDigestInputs{
                      session.GetWorldSystem()->snapshot_region_authority(0, 0), {}, true}),
              expected);
    EXPECT_EQ(session.GetActiveRegionLedger().canonical_bytes(), expected_ledger);
}

TEST(GameSessionHeadlessWorldTest, ExternalChunkAdoptionSharesResidentBudget) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateTransientWorld("Resident budget", "1337", "default"));
    auto* world = session.GetWorldSystem();
    constexpr auto budget = Luminumbra::Systems::SHIELD_WorldSystem::kActiveChunkBudget;
    for (std::size_t index = 0; index < budget; ++index) {
        auto chunk =
            std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(static_cast<int>(index), 0, 0));
        ASSERT_TRUE(world->adopt_streamed_chunk(chunk));
    }
    EXPECT_FALSE(world->adopt_streamed_chunk(
        std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(static_cast<int>(budget), 0, 0))));
    EXPECT_EQ(world->snapshot_streamed_chunks().size(), budget);
}

TEST(GameSessionHeadlessWorldTest, FreshNaturalWaterAndFrozenRenderWaterShareInitialization) {
    using Luminumbra::IVec3;
    using Luminumbra::Vec3;
    const HeadlessRoot root;
    // Keep both locations submerged without depending on a particular noise sample.
    const auto preset_path = root.path() / "worlds/atlas/presets/default.json";
    auto preset = nlohmann::json::parse(ReadClockTestFile(preset_path));
    preset["generation_params"]["terrain"]["height_offset"] = -256.0;
    {
        std::ofstream output(preset_path);
        output << preset.dump();
        ASSERT_TRUE(output.good());
    }
    JobSystem jobs;
    jobs.startup(1);
    GameSession near_camera, far_camera;
    const Vec3 anchor(81928, 8, 81928);
    for (auto* session : {&near_camera, &far_camera}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetActiveRegionsEnabled(true);
        session->SetWeatherRainEnabled(false);
        ASSERT_TRUE(session->CreateTransientWorld("Natural water", "1337", "default"));
        session->SetReplicatedSimulationAnchors({anchor});
    }
    auto* near_world = near_camera.GetWorldSystem();
    auto* far_world = far_camera.GetWorldSystem();
    ASSERT_GT(near_world->WaterLevelAt(anchor.x, anchor.z),
              near_world->GetTerrainHeightAt(anchor.x, anchor.z));
    const auto natural_coords = Luminumbra::Systems::SHIELD_WorldSystem::world_to_chunk_coords(
        Vec3(anchor.x, near_world->WaterLevelAt(anchor.x, anchor.z), anchor.z));
    const IVec3 far_coords(-5120, natural_coords.y, -5120);
    far_world->dispatch_generation_jobs(std::vector<IVec3>{far_coords});
    far_world->wait_for_streaming_jobs();
    auto frozen = far_world->find_streamed_chunk(far_coords);
    ASSERT_NE(frozen, nullptr);
    ASSERT_TRUE(frozen->has_water_sim.load());
    ASSERT_NE(frozen->sdf_provenance(), Luminumbra::ChunkSdfProvenance::LoadedOrEdited);
    Luminumbra::WorldStreamingState frozen_state;
    frozen_state.insert_chunk(frozen);
    const auto frozen_water = P::ComputeWorldStreamingStateSubHashes(frozen_state).water;
    Luminumbra::World::MarchingCubes::GenerateWaterMesh(
        *far_camera.GetWaterSystem(), *far_world, *frozen);
    EXPECT_FALSE(frozen->water_mesh_vertices.empty());
    EXPECT_FALSE(frozen->water_mesh_indices.empty());
    for (int tick = 0; tick < 4; ++tick) {
        for (auto* session : {&near_camera, &far_camera}) {
            ASSERT_EQ(session->TickSimulation(session->GetSimulationClock().fixed_dt()), 1u);
            ASSERT_FALSE(session->GetRegionSchedulerFault());
        }
        EXPECT_EQ(P::ComputeWorldStreamingStateSubHashes(frozen_state).water, frozen_water);
        const auto near_hashes = near_world->compute_chunk_hashes();
        const auto far_hashes = far_world->compute_chunk_hashes();
        EXPECT_EQ(near_hashes.world, far_hashes.world);
        EXPECT_EQ(near_hashes.sub.water, far_hashes.sub.water);
        EXPECT_EQ(near_camera.GetActiveRegionLedger().canonical_bytes(),
                  far_camera.GetActiveRegionLedger().canonical_bytes());
    }
    auto natural = near_world->find_streamed_chunk(natural_coords);
    ASSERT_NE(natural, nullptr);
    EXPECT_TRUE(natural->has_water_sim.load());
    EXPECT_FALSE(natural->water_depth_mm.empty());
    EXPECT_GT(*std::max_element(natural->water_depth_mm.begin(), natural->water_depth_mm.end()), 0);
    Luminumbra::World::MarchingCubes::GenerateWaterMesh(
        *near_camera.GetWaterSystem(), *near_world, *natural);
    EXPECT_FALSE(natural->water_mesh_vertices.empty());
    EXPECT_FALSE(natural->water_mesh_indices.empty());
    EXPECT_EQ(near_world->simulation_chunk_count(), 18u);
    EXPECT_EQ(far_world->simulation_chunk_count(), 18u);
}

TEST(GameSessionHeadlessWorldTest, SubmergedTerrainCollisionIgnoresRenderSchedule) {
    using Luminumbra::Vec3;
    using World = Luminumbra::Systems::SHIELD_WorldSystem;
    const HeadlessRoot root;
    const auto preset_path = root.path() / "worlds/atlas/presets/default.json";
    auto preset = nlohmann::json::parse(ReadClockTestFile(preset_path));
    preset["generation_params"]["terrain"]["height_offset"] = -256.0;
    {
        std::ofstream output(preset_path);
        output << preset.dump();
        ASSERT_TRUE(output.good());
    }
    JobSystem jobs;
    jobs.startup(1);
    GameSession rendered, headless;
    const Vec3 anchor(81928, 8, 81928);
    for (auto* session : {&rendered, &headless}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetActiveRegionsEnabled(true);
        session->SetWeatherRainEnabled(false);
        ASSERT_TRUE(session->CreateTransientWorld("Submerged collision", "1337", "default"));
        session->SetReplicatedSimulationAnchors({anchor});
        session->GetWorldSystem()->debug_set_streaming_radius_cap(1);
    }
    for (int tick = 0; tick < 2; ++tick) {
        rendered.GetWorldSystem()->update(
            rendered.GetRegistry(), anchor, rendered.GetPhysicsSystem());
        rendered.GetWorldSystem()->wait_for_streaming_jobs();
        for (auto* session : {&rendered, &headless}) {
            // Admission must provide collision before any simulation/physics tick.
            session->PrepareSimulationCollisions();
            ASSERT_FALSE(session->GetRegionSchedulerFault());
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const float x = anchor.x + static_cast<float>(dx * Luminumbra::CHUNK_SIZE_X);
                    const float z = anchor.z + static_cast<float>(dz * Luminumbra::CHUNK_SIZE_Z);
                    auto* world = session->GetWorldSystem();
                    const float ground = world->GetTerrainHeightAt(x, z);
                    const auto ground_coords = World::world_to_chunk_coords(Vec3(x, ground, z));
                    const auto water_coords =
                        World::world_to_chunk_coords(Vec3(x, world->WaterLevelAt(x, z), z));
                    ASSERT_LT(ground_coords.y, water_coords.y);
                    ASSERT_NE(world->find_streamed_chunk(ground_coords), nullptr);
                    EXPECT_TRUE(world->find_streamed_chunk(ground_coords)->has_collision.load());
                    const auto hit = session->GetPhysicsSystem()->audio_raycast(
                        Vec3(x, ground + 2.0f, z), Vec3(x, ground - 2.0f, z));
                    ASSERT_TRUE(hit.hit);
                    EXPECT_NEAR(hit.hit_point.y, ground, 0.05f);
                }
            }
            ASSERT_EQ(session->TickSimulation(session->GetSimulationClock().fixed_dt()), 1u);
        }
        EXPECT_EQ(rendered.GetWorldSystem()->compute_chunk_hashes().world,
                  headless.GetWorldSystem()->compute_chunk_hashes().world);
        EXPECT_EQ(rendered.GetActiveRegionLedger().canonical_bytes(),
                  headless.GetActiveRegionLedger().canonical_bytes());
    }
}

TEST(GameSessionHeadlessWorldTest, UnsupportedEntryRadiiRefuseBeforeCollisionAdmission) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateTransientWorld("Entry radius refusal", "1337", "default"));
    auto* world = session.GetWorldSystem();
    world->wait_for_streaming_jobs();
    const auto before = world->compute_chunk_hashes();
    const auto ledger = session.GetActiveRegionLedger().canonical_bytes();
    const auto chunks = world->snapshot_streamed_chunks();
    std::vector<bool> collision_flags;
    for (const auto& chunk : chunks)
        collision_flags.push_back(chunk->has_collision.load());
    for (const auto& [horizon, collision] : {std::pair{-1, 0},
                                             std::pair{13, 0},
                                             std::pair{0, -1},
                                             std::pair{0, 13},
                                             std::pair{std::numeric_limits<int>::max(), 0}}) {
        EXPECT_FALSE(session.EnsureWorldEntryReady(horizon, collision));
        EXPECT_EQ(world->compute_chunk_hashes().world, before.world);
        EXPECT_EQ(session.GetActiveRegionLedger().canonical_bytes(), ledger);
        EXPECT_EQ(world->snapshot_streamed_chunks().size(), chunks.size());
        for (std::size_t index = 0; index < chunks.size(); ++index)
            EXPECT_EQ(chunks[index]->has_collision.load(), collision_flags[index]);
        EXPECT_EQ(session.GetWorldClock().tick(), 0u);
        EXPECT_FALSE(session.GetRegionSchedulerFault());
    }
}

TEST(GameSessionHeadlessWorldTest, StartupAdmissionReadiesCollisionBeforeTheFirstTick) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    EXPECT_FALSE(session.EnsureWorldEntryReady(1, 1));
    ASSERT_TRUE(session.CreateTransientWorld("Startup collision", "1337", "default"));
    auto* world = session.GetWorldSystem();
    ASSERT_TRUE(session.EnsureWorldEntryReady(1, 1));
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    std::size_t colliders = 0;
    for (const auto& chunk : world->snapshot_streamed_chunks())
        colliders += chunk->has_collision.load() ? 1u : 0u;
    EXPECT_GE(colliders, 9u);
    EXPECT_EQ(session.GetWorldClock().tick(), 0u);
    EXPECT_EQ(session.GetActiveRegionLedger().tick(), 0u);
    // Empty replicated ownership is explicit, even at startup.
    session.SetReplicatedSimulationAnchors({});
    session.PrepareSimulationCollisions();
    EXPECT_EQ(world->simulation_chunk_count(), 0u);
    for (const auto& chunk : world->snapshot_streamed_chunks())
        EXPECT_FALSE(chunk->has_collision.load());
    // A malformed startup region must refuse entry before any clock advances.
    const auto directory = root.path() / "entry-corrupt-authority";
    const auto spawn = session.GetMetadata().spawnPoint;
    const auto key = Luminumbra::world::ActiveRegionLedger::region_at(spawn);
    const auto path = P::WorldSaveService::region_file_path(directory, key.x, key.z);
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << "partial";
    world->set_authority_directory(directory);
    session.SetReplicatedSimulationAnchors({spawn});
    EXPECT_FALSE(session.EnsureWorldEntryReady(1, 1));
    EXPECT_TRUE(session.GetRegionSchedulerFault());
    EXPECT_EQ(session.GetWorldClock().tick(), 0u);
    EXPECT_EQ(session.GetActiveRegionLedger().tick(), 0u);
}

TEST(GameSessionHeadlessWorldTest, GeneratedWaterAuthoritySurvivesAcknowledgementAndEviction) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Generated water ownership", "1337", "default"));
    session.SetReplicatedSimulationAnchors({});
    auto* world = session.GetWorldSystem();
    const Luminumbra::IVec3 coords(-1279, 0, -1279);
    auto chunk = FlowingRegionChunk(coords);
    chunk->mark_sdf_generated_current_params();
    chunk->mark_water_data_dirty();
    ASSERT_TRUE(world->adopt_streamed_chunk(chunk));
    const auto hash = world->compute_chunk_hashes().world;
    const auto digest = Digest({}, {-40, -40}, {chunk});
    EXPECT_NE(digest, Digest({}, {-40, -40}, {}));
    ASSERT_TRUE(session.SaveWorldState());
    EXPECT_FALSE(chunk->is_voxel_data_dirty());
    EXPECT_TRUE(chunk->has_water_authority());
    EXPECT_EQ(chunk->sdf_provenance(), Luminumbra::ChunkSdfProvenance::GeneratedCurrentParams);
    chunk->water_depth_mm[0] += 123;
    chunk->mark_water_data_dirty();
    const auto captured_hash = world->compute_chunk_hashes().world;
    EXPECT_NE(captured_hash, hash);
    // No cadence capture observes this mutation before eviction writes it through.
    chunk->water_depth_mm[0] += 1;
    chunk->mark_water_data_dirty();
    const auto newer_hash =
        P::BindWaterScheduler(P::WorldSaveService::authority_hashes({}, {chunk}),
                              world->GetWaterSimWindowCursor())
            .world;
    EXPECT_NE(newer_hash, captured_hash);
    const auto newer_digest = Digest({}, {-40, -40}, {chunk});
    const auto depth = chunk->water_depth_mm;
    const std::weak_ptr<Luminumbra::Chunk> ownership = chunk;
    chunk.reset();
    world->debug_set_streaming_radius_cap(1);
    for (int frame = 0; frame < 4; ++frame) {
        world->update(session.GetRegistry(), session.GetMetadata().spawnPoint, nullptr);
        world->wait_for_streaming_jobs();
    }
    ASSERT_FALSE(world->authority_fault());
    EXPECT_TRUE(ownership.expired());
    EXPECT_EQ(world->compute_chunk_hashes().world, newer_hash);
    EXPECT_EQ(Digest(session.GetWorldSaveDir(), {-40, -40}, {}), newer_digest);
    world->dispatch_generation_jobs(std::vector<Luminumbra::IVec3>{coords});
    world->wait_for_streaming_jobs();
    const auto restored = world->find_streamed_chunk(coords);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->water_depth_mm, depth);
    EXPECT_TRUE(restored->has_water_authority());
    EXPECT_EQ(restored->sdf_provenance(), Luminumbra::ChunkSdfProvenance::GeneratedCurrentParams);
    EXPECT_EQ(world->compute_chunk_hashes().world, newer_hash);
}

TEST(GameSessionHeadlessWorldTest, LoadPrimesAuthorityHashesBeforeSchedulerFaultInjection) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Loaded authority hashes", "1337", "default"));
    auto* world = session.GetWorldSystem();
    const auto save = session.GetWorldSaveDir();
    ASSERT_TRUE(world->adopt_streamed_chunk(FlowingRegionChunk({64, 0, 64})));
    ASSERT_TRUE(session.SaveWorldState());
    const auto expected = world->compute_chunk_hashes().world;
    ASSERT_TRUE(session.LoadWorldState());
    // The first observation after load must use the accepted snapshot even if
    // a later region provider encounters a new, malformed durable file.
    const auto fault = P::WorldSaveService::region_file_path(save, 9, 9);
    fs::create_directories(fault.parent_path());
    std::ofstream(fault, std::ios::binary) << "partial";
    EXPECT_NO_THROW(EXPECT_EQ(world->compute_chunk_hashes().world, expected));
    EXPECT_THROW(P::WorldSaveService::authority_hashes(save, {}), std::runtime_error);
    ASSERT_TRUE(fs::remove(fault));
    EXPECT_EQ(P::BindWaterScheduler(
                  P::WorldSaveService::authority_hashes(save, world->snapshot_streamed_chunks()),
                  world->GetWaterSimWindowCursor())
                  .world,
              expected);
}

TEST(GameSessionHeadlessWorldTest, CachedAuthorityHashMatchesFullRecomputeWithoutDurableReads) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Authority hash cache", "1337", "default"));
    session.SetReplicatedSimulationAnchors({});
    auto* world = session.GetWorldSystem();
    const auto save = session.GetWorldSaveDir();
    auto live = FlowingRegionChunk({64, 0, 64});
    auto durable = FlowingRegionChunk({-64, 0, -64});
    ASSERT_TRUE(P::WorldSaveService::persist_chunk_authority(save, durable));
    ASSERT_TRUE(world->adopt_streamed_chunk(live));
    const auto check = [&] {
        const auto cached = world->compute_chunk_hashes();
        const auto full = P::BindWaterScheduler(
            P::WorldSaveService::authority_hashes(save, world->snapshot_streamed_chunks()),
            world->GetWaterSimWindowCursor());
        EXPECT_EQ(cached.world, full.world);
        EXPECT_EQ(cached.sub.terrain, full.sub.terrain);
        EXPECT_EQ(cached.sub.water, full.sub.water);
    };
    check();
    for (int mutation = 0; mutation < 3; ++mutation) {
        // All authoritative buffer writers advance voxel and water hash revisions.
        live->heightmap_data[0] += 1.0f;
        live->water_depth_mm[0] += 1;
        live->mark_voxel_data_dirty();
        live->mark_water_data_dirty();
        check();
    }
    const auto before_cursor = world->compute_chunk_hashes();
    world->SetWaterSimWindowCursor(std::numeric_limits<std::size_t>::max());
    const auto after_cursor = world->compute_chunk_hashes();
    EXPECT_EQ(after_cursor.water_scheduler_cursor, std::numeric_limits<std::size_t>::max());
    EXPECT_NE(after_cursor.world, before_cursor.world);
    EXPECT_NE(after_cursor.sub.water, before_cursor.sub.water);
    EXPECT_EQ(after_cursor.sub.terrain, before_cursor.sub.terrain);
    EXPECT_EQ(after_cursor.sub.mesh, before_cursor.sub.mesh);
    // The same live/durable cache still agrees with the independent full oracle.
    check();
    const auto path = P::WorldSaveService::region_file_path(save, -2, -2);
    const auto original = ReadClockTestFile(path);
    std::ofstream(path, std::ios::binary) << "partial";
    EXPECT_NO_THROW(world->compute_chunk_hashes());
    EXPECT_THROW(P::WorldSaveService::authority_hashes(save, {}), std::runtime_error);
    // A cold failure cannot publish a partial cache.
    world->clear_world(nullptr);
    EXPECT_THROW(world->compute_chunk_hashes(), std::runtime_error);
    std::ofstream(path, std::ios::binary) << original;
    check();
    ASSERT_TRUE(world->adopt_streamed_chunk(live));
    check();
    const auto alternate = root.path() / "cached-authority-alternate";
    ASSERT_TRUE(session.SaveWorldStateTo(alternate));
    EXPECT_EQ(world->compute_chunk_hashes().world,
              P::BindWaterScheduler(P::WorldSaveService::authority_hashes(
                                        alternate, world->snapshot_streamed_chunks()),
                                    world->GetWaterSimWindowCursor())
                  .world);
    // Root replacement drops the previous root's durable-only contributions.
    const auto empty = root.path() / "empty-authority-root";
    world->set_authority_directory(empty);
    EXPECT_EQ(world->compute_chunk_hashes().world,
              P::BindWaterScheduler(
                  P::WorldSaveService::authority_hashes(empty, world->snapshot_streamed_chunks()),
                  world->GetWaterSimWindowCursor())
                  .world);
    world->set_authority_directory(alternate);
    EXPECT_EQ(world->compute_chunk_hashes().world,
              P::BindWaterScheduler(P::WorldSaveService::authority_hashes(
                                        alternate, world->snapshot_streamed_chunks()),
                                    world->GetWaterSimWindowCursor())
                  .world);
}

TEST(GameSessionHeadlessWorldTest, LegacyInitializedWaterMigratesWithoutChangingSavedFields) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession legacy, loaded, resumed;
    for (auto* session : {&legacy, &loaded, &resumed}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
    }
    legacy.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(legacy.CreateWorld("Legacy initialized water", "1337", "default"));
    const auto original = FlowingRegionChunk({0, 0, 0});
    original->mark_sdf_generated_current_params();
    ASSERT_TRUE(original->has_water_sim.load());
    ASSERT_FALSE(original->has_water_authority());
    Luminumbra::WorldStreamingState state;
    state.insert_chunk(original);
    ASSERT_FALSE(nlohmann::json::parse(P::SerializeWorldStreamingStateSnapshotJson(state))
                     .at("chunks")
                     .at(0)
                     .contains("water_authority"));
    P::WorldSaveService service;
    ASSERT_TRUE(service.save_world(state, legacy.GetWorldSaveDir()));
    const auto path = P::WorldSaveService::region_file_path(legacy.GetWorldSaveDir(), 0, 0);
    const auto before_load = ReadClockTestFile(path);
    // Authority admission and cold hash capture must see legacy water even
    // when the chunk is not part of the initial resident load.
    P::WorldSaveService::AuthorityReader reader;
    ASSERT_TRUE(reader.open(path));
    ASSERT_EQ(reader.next_id(), original->get_id());
    const auto durable = reader.read(original->get_id());
    ASSERT_NE(durable, nullptr);
    EXPECT_TRUE(durable->has_water_authority());
    EXPECT_EQ(durable->water_depth_mm, original->water_depth_mm);
    EXPECT_EQ(durable->water_bed_mm, original->water_bed_mm);
    EXPECT_EQ(durable->water_edge_flux, original->water_edge_flux);
    reader = {};
    loaded.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(loaded.LoadWorld(legacy.GetMetadata().worldId));
    EXPECT_EQ(ReadClockTestFile(path), before_load);
    const auto restored = loaded.GetWorldSystem()->find_streamed_chunk({0, 0, 0});
    ASSERT_NE(restored, nullptr);
    EXPECT_TRUE(restored->has_water_authority());
    EXPECT_EQ(restored->water_depth_mm, original->water_depth_mm);
    EXPECT_EQ(restored->water_bed_mm, original->water_bed_mm);
    EXPECT_EQ(restored->water_edge_flux, original->water_edge_flux);
    ASSERT_TRUE(loaded.SaveWorldState());
    resumed.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(resumed.LoadWorld(legacy.GetMetadata().worldId));
    const auto saved_again = resumed.GetWorldSystem()->find_streamed_chunk({0, 0, 0});
    ASSERT_NE(saved_again, nullptr);
    EXPECT_TRUE(saved_again->has_water_authority());
    EXPECT_EQ(saved_again->water_depth_mm, original->water_depth_mm);
    EXPECT_EQ(saved_again->water_bed_mm, original->water_bed_mm);
    EXPECT_EQ(saved_again->water_edge_flux, original->water_edge_flux);
}

TEST(GameSessionHeadlessWorldTest, LegacyWaterIndexRejectsPartialDataAndIgnoresMeshes) {
    const HeadlessRoot root;
    const auto path = P::WorldSaveService::region_file_path(root.path(), 0, 0);
    Luminumbra::WorldStreamingState state;
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0));
    chunk->water_mesh_vertices.resize(3);
    state.insert_chunk(chunk);
    P::WorldSaveService service;
    ASSERT_TRUE(service.save_world(state, root.path()));
    P::WorldSaveService::AuthorityReader reader;
    ASSERT_TRUE(reader.open(path));
    EXPECT_FALSE(reader.next_id());
    reader = {};
    const P::WorldSaveService::ContainerRecord partial{chunk->get_id(), 0, 2, "{\"chunks\":["};
    ASSERT_TRUE(P::WorldSaveService::upsert_container_records(path, {partial}));
    const auto before = ReadClockTestFile(path);
    EXPECT_FALSE(reader.open(path));
    EXPECT_EQ(ReadClockTestFile(path), before);
}

TEST(GameSessionHeadlessWorldTest, EvictionSnapshotExcludesWorkerStagingAndPreservesAuthority) {
    auto chunk = FlowingRegionChunk({0, 0, 0});
    chunk->pending_mesh_indices = {0, 1, 2};
    chunk->pending_water_mesh_indices = {2, 1, 0};
    Luminumbra::WorldStreamingState state;
    state.insert_chunk(chunk);
    const auto full = nlohmann::json::parse(P::SerializeWorldStreamingStateSnapshotJson(state));
    const auto authority = nlohmann::json::parse(P::SerializeChunkAuthoritySnapshotJson(*chunk));
    EXPECT_FALSE(full.at("chunks").at(0).at("pending_mesh_indices").empty());
    EXPECT_TRUE(authority.at("chunks").at(0).at("pending_mesh_indices").empty());
    EXPECT_TRUE(authority.at("chunks").at(0).at("pending_water_mesh_indices").empty());
    Luminumbra::WorldStreamingState loaded;
    std::vector<std::string> errors;
    ASSERT_TRUE(P::LoadWorldStreamingStateSnapshotJson(authority.dump(), loaded, errors));
    const auto restored = loaded.find_chunk(chunk->get_id());
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(P::WorldSaveService::authority_hashes({}, {chunk}).world,
              P::WorldSaveService::authority_hashes({}, {restored}).world);
}

TEST(GameSessionHeadlessWorldTest, AuthoritySnapshotsKeepTerrainWaterAndRegenerableOwnership) {
    for (const bool terrain_edited : {false, true}) {
        for (const bool water_owned : {false, true}) {
            auto chunk = FlowingRegionChunk({0, 0, 0});
            chunk->mark_sdf_generated_current_params();
            if (terrain_edited)
                chunk->mark_voxel_data_dirty();
            if (water_owned)
                chunk->mark_water_data_dirty();
            const auto snapshot = P::SerializeChunkAuthoritySnapshotJson(*chunk);
            Luminumbra::WorldStreamingState loaded;
            std::vector<std::string> errors;
            ASSERT_TRUE(P::LoadWorldStreamingStateSnapshotJson(snapshot, loaded, errors));
            const auto restored = loaded.find_chunk(chunk->get_id());
            ASSERT_NE(restored, nullptr);
            EXPECT_EQ(restored->sdf_provenance(), chunk->sdf_provenance());
            EXPECT_EQ(restored->has_water_authority(), water_owned);
            EXPECT_EQ(restored->has_simulation_authority(), terrain_edited || water_owned);
            EXPECT_FALSE(restored->is_voxel_data_dirty());
            EXPECT_EQ(restored->water_depth_mm, chunk->water_depth_mm);
            EXPECT_EQ(restored->water_edge_flux, chunk->water_edge_flux);
            EXPECT_EQ(P::WorldSaveService::authority_hashes({}, {restored}).world,
                      P::WorldSaveService::authority_hashes({}, {chunk}).world);
        }
    }
}

TEST(GameSessionHeadlessWorldTest, MalformedAuthorityOwnershipRefusesTheWholeSnapshot) {
    const auto chunk = FlowingRegionChunk({0, 0, 0});
    const auto valid = nlohmann::json::parse(P::SerializeChunkAuthoritySnapshotJson(*chunk));
    for (const auto& invalid :
         {nlohmann::json(nullptr),
          nlohmann::json("edited"),
          nlohmann::json{{"terrain_edited", true}},
          nlohmann::json{{"terrain_edited", 1}, {"water_owned", false}},
          nlohmann::json{{"terrain_edited", true}, {"water_owned", "false"}}}) {
        auto document = valid;
        document["chunks"][0]["authority"] = invalid;
        Luminumbra::WorldStreamingState loaded;
        std::vector<std::string> errors;
        EXPECT_FALSE(P::LoadWorldStreamingStateSnapshotJson(document.dump(), loaded, errors));
        EXPECT_TRUE(loaded.empty());
        EXPECT_FALSE(errors.empty());
        errors.clear();
        EXPECT_FALSE(
            P::ValidateWorldStreamingChunkFormatJson(document["chunks"][0].dump(), errors));
        EXPECT_FALSE(errors.empty());
    }
    auto inconsistent = valid;
    inconsistent["chunks"][0]["water_authority"] = "generated";
    Luminumbra::WorldStreamingState loaded;
    std::vector<std::string> errors;
    EXPECT_FALSE(P::LoadWorldStreamingStateSnapshotJson(inconsistent.dump(), loaded, errors));
    EXPECT_TRUE(loaded.empty());
}

TEST(GameSessionHeadlessWorldTest, UnchangedDurableOwnersEvictWithoutAnotherRegionWrite) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Durable eviction", "1337", "default"));
    session.SetReplicatedSimulationAnchors({});
    const Luminumbra::IVec3 coords(1280, 0, -1280);
    ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(FlowingRegionChunk(coords)));
    ASSERT_TRUE(session.SaveWorldState());
    ASSERT_TRUE(session.LoadWorldState());
    session.SetReplicatedSimulationAnchors({});
    auto* world = session.GetWorldSystem();
    world->debug_set_streaming_radius_cap(1);
    P::WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    for (int frame = 0; frame < 4; ++frame) {
        world->update(session.GetRegistry(), session.GetMetadata().spawnPoint, nullptr);
        world->wait_for_streaming_jobs();
    }
    P::WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
    EXPECT_FALSE(world->authority_fault());
    EXPECT_EQ(world->find_streamed_chunk(coords), nullptr);
}

TEST(GameSessionHeadlessWorldTest, DurableWaterBookkeepingChangesStillRequireAnEvictionWrite) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateWorld("Changed durable eviction", "1337", "default"));
    session.SetReplicatedSimulationAnchors({});
    const Luminumbra::IVec3 coords(1280, 0, -1280);
    ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(FlowingRegionChunk(coords)));
    ASSERT_TRUE(session.SaveWorldState());
    ASSERT_TRUE(session.LoadWorldState());
    session.SetReplicatedSimulationAnchors({});
    auto* world = session.GetWorldSystem();
    const auto chunk = world->find_streamed_chunk(coords);
    ASSERT_NE(chunk, nullptr);
    ASSERT_FALSE(chunk->is_voxel_data_dirty());
    ++chunk->ticks_below_threshold;
    world->debug_set_streaming_radius_cap(1);
    P::WorldSaveService::set_interrupt_before_region_replace_for_testing(true);
    for (int frame = 0; frame < 4; ++frame) {
        world->update(session.GetRegistry(), session.GetMetadata().spawnPoint, nullptr);
        world->wait_for_streaming_jobs();
    }
    P::WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
    EXPECT_TRUE(world->authority_fault());
    EXPECT_EQ(world->find_streamed_chunk(coords), chunk);
}

TEST(GameSessionHeadlessWorldTest, AuthorityHashObservesEverySolverBookkeepingScalar) {
    auto chunk = FlowingRegionChunk({0, 0, 0});
    P::AuthorityHashCache cache;
    cache.update(chunk);
    const auto verify_change = [&] {
        const auto before = cache.hashes();
        const auto projections = cache.projection_count();
        cache.update(chunk);
        EXPECT_NE(cache.hashes().sub.water, before.sub.water);
        EXPECT_NE(cache.hashes().world, before.world);
        EXPECT_EQ(cache.hashes().sub.terrain, before.sub.terrain);
        EXPECT_EQ(cache.projection_count(), projections + 1);
        EXPECT_EQ(cache.hashes().world, P::WorldSaveService::authority_hashes({}, {chunk}).world);
        cache.update(chunk);
        EXPECT_EQ(cache.projection_count(), projections + 1);
    };
    ++chunk->ticks_below_threshold;
    verify_change();
    chunk->max_water_delta_last_tick += 0.5f;
    verify_change();
    chunk->is_water_sleeping.store(true);
    verify_change();
    chunk->current_water_resolution.store(8);
    verify_change();
    chunk->has_water_sim.store(false);
    verify_change();
}

TEST(GameSessionHeadlessWorldTest, AuthorityHashProjectsOnlyChangedOwnersAndWaterRevisions) {
    P::AuthorityHashCache cache;
    cache.update(nullptr);
    EXPECT_EQ(cache.projection_count(), 0u);
    auto first = FlowingRegionChunk({0, 0, 0});
    auto second = FlowingRegionChunk({1, 0, 0});
    cache.update(first);
    cache.update(second);
    const auto baseline = cache.hashes().world;
    // A render cache cannot retire durable authority; only a committed migration can.
    cache.update(std::make_shared<Luminumbra::Chunk>(first->get_coords()));
    EXPECT_EQ(cache.hashes().world, baseline);
    EXPECT_EQ(cache.projection_count(), 2u);
    for (int capture = 0; capture < 8; ++capture) {
        cache.update(first);
        cache.update(second);
        EXPECT_EQ(cache.hashes().world, baseline);
    }
    EXPECT_EQ(cache.projection_count(), 2u);
    first->water_edge_flux[0] += 1;
    first->mark_water_data_dirty();
    first->clear_voxel_data_dirty();
    cache.update(first);
    cache.update(second);
    EXPECT_EQ(cache.projection_count(), 3u);
    EXPECT_NE(cache.hashes().world, baseline);
    EXPECT_EQ(cache.hashes().world,
              P::WorldSaveService::authority_hashes({}, {first, second}).world);
    // A different owner with the same id and revision must replace the old contribution.
    auto replacement = FlowingRegionChunk({0, 0, 0});
    replacement->water_depth_mm[0] += 17;
    cache.update(replacement);
    EXPECT_EQ(cache.projection_count(), 4u);
    EXPECT_EQ(cache.hashes().world,
              P::WorldSaveService::authority_hashes({}, {replacement, second}).world);
}

TEST(GameSessionHeadlessWorldTest, ExpiredAuthorityOwnerCannotAcknowledgeReplacementOrEmptyInput) {
    P::AuthorityHashCache cache;
    auto original = FlowingRegionChunk({0, 0, 0});
    const auto stamp = original->authority_stamp();
    cache.update(original);
    const auto before = cache.hashes().world;
    const std::weak_ptr<Luminumbra::Chunk> retired = original;
    original.reset();
    ASSERT_TRUE(retired.expired());
    cache.update(nullptr);
    cache.update(std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(0, 0, 0)));
    EXPECT_EQ(cache.hashes().world, before);
    EXPECT_EQ(cache.projection_count(), 1u);

    auto replacement = FlowingRegionChunk({0, 0, 0});
    ASSERT_EQ(replacement->authority_stamp(), stamp);
    replacement->heightmap_data[0] += 2.0f;
    replacement->water_depth_mm[0] += 17;
    // An equal stamp belongs to its particular owner; it cannot authorize
    // reusing the expired owner's terrain or water contribution.
    cache.update(replacement);
    EXPECT_NE(cache.hashes().world, before);
    EXPECT_EQ(cache.projection_count(), 2u);
    EXPECT_EQ(cache.hashes().world, P::WorldSaveService::authority_hashes({}, {replacement}).world);
    cache.update(replacement);
    EXPECT_EQ(cache.projection_count(), 2u);
}

TEST(GameSessionHeadlessWorldTest, WaterBedEditsAdmitOwnersOutsideTheWalkingHorizon) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    ASSERT_TRUE(session.CreateWorld("Remote bed edit", "1337", "default"));
    auto* world = session.GetWorldSystem();
    session.PrepareSimulationCollisions();
    const Luminumbra::Vec3 remote(3208, 400, 3208);
    const Luminumbra::IVec3 owner_coords(200, 0, 200);
    auto owner = FlowingRegionChunk(owner_coords);
    owner->mark_sdf_generated_current_params();
    ASSERT_TRUE(world->adopt_streamed_chunk(owner));
    const auto bed = owner->water_bed_mm;
    const auto terrain = owner->sdf_data;
    const auto before = world->compute_chunk_hashes().world;
    EXPECT_GT(world->EditTerrainBed(remote, -500, 8.0f), 0);
    EXPECT_NE(owner->water_bed_mm, bed);
    EXPECT_EQ(world->find_streamed_chunk(owner_coords), owner);
    EXPECT_EQ(owner->sdf_data, terrain);
    EXPECT_TRUE(owner->has_water_authority());
    EXPECT_NE(world->compute_chunk_hashes().world, before);
    const auto edited = owner->water_bed_mm;
    EXPECT_EQ(world->EditTerrainBed(remote, 0, 8.0f), 0);
    EXPECT_EQ(world->EditTerrainBed(remote, 1, std::numeric_limits<float>::infinity()), 0);
    EXPECT_EQ(world->EditTerrainBed(remote, 1, -1.0f), 0);
    EXPECT_EQ(world->EditTerrainBed(remote, 1, 100000.0f), 0);
    EXPECT_EQ(
        world->EditTerrainBed({remote.x, std::numeric_limits<float>::max(), remote.z}, 1, 8.0f), 0);
    EXPECT_EQ(owner->water_bed_mm, edited);
    ASSERT_TRUE(session.SaveWorldState());
    ASSERT_TRUE(session.LoadWorldState());
    owner = world->find_streamed_chunk(owner_coords);
    ASSERT_NE(owner, nullptr);
    EXPECT_EQ(owner->water_bed_mm, edited);
    // An absent owner must also be admitted, initialized and edited.
    EXPECT_GT(world->EditTerrainBed(remote + Luminumbra::Vec3(64, 0, 0), 10, 4.0f), 0);
    ASSERT_NE(world->find_streamed_chunk({204, 0, 200}), nullptr);
    EXPECT_TRUE(world->find_streamed_chunk({204, 0, 200})->has_water_authority());
}

TEST(GameSessionHeadlessWorldTest, WaterBedEditBoundsAndEmptyOwnersAreSafe) {
    Luminumbra::Systems::WaterSystem water(nullptr, nullptr);
    const Luminumbra::Vec3 center(8, 0, 8);
    EXPECT_EQ(water.EditTerrainBed(center, 1, 8.0f), 0);
    std::unordered_map<Luminumbra::ChunkID, std::shared_ptr<Luminumbra::Chunk>> owners;
    EXPECT_EQ(water.EditTerrainBed(center, 1, 8.0f, &owners), 0);
    auto chunk = FlowingRegionChunk({0, 0, 0});
    owners.emplace(chunk->get_id(), chunk);
    const auto count = chunk->water_bed_mm.size();
    const auto check = [&](std::int32_t initial, std::int32_t delta) {
        chunk->water_bed_mm.assign(count, initial);
        EXPECT_EQ(water.EditTerrainBed(center, delta, 32.0f, &owners), 0);
        EXPECT_EQ(chunk->water_bed_mm, std::vector<std::int32_t>(count, initial));
    };
    check(std::numeric_limits<std::int32_t>::max(), 1);
    check(std::numeric_limits<std::int32_t>::min(), -1);
    EXPECT_EQ(water.EditTerrainBed(center, 1, 0.0f, &owners), 0);
    EXPECT_EQ(
        water.EditTerrainBed({std::numeric_limits<float>::quiet_NaN(), 0, 0}, 1, 32.0f, &owners),
        0);
    EXPECT_EQ(water.EditTerrainBed(center, 100, 32.0f, &owners), static_cast<int>(count));
    EXPECT_TRUE(chunk->has_water_authority());
}

TEST(GameSessionHeadlessWorldTest, WaterOwnerReadFailureRefusesVoxelAndBedEdits) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    ASSERT_TRUE(session.CreateWorld("Refused remote edit", "1337", "default"));
    auto* world = session.GetWorldSystem();
    const Luminumbra::IVec3 coords(200, 25, 200);
    const auto terrain = std::make_shared<Luminumbra::Chunk>(coords);
    constexpr std::size_t kSamples = static_cast<std::size_t>(Luminumbra::CHUNK_SIZE_X + 1) *
                                     (Luminumbra::CHUNK_SIZE_Y + 1) *
                                     (Luminumbra::CHUNK_SIZE_Z + 1);
    terrain->sdf_data.assign(kSamples, -1.0f);
    terrain->mark_voxel_data_dirty();
    ASSERT_TRUE(world->adopt_streamed_chunk(terrain));
    const auto before = terrain->sdf_data;
    const auto path = P::WorldSaveService::region_file_path(session.GetWorldSaveDir(), 6, 6);
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << "partial";
    const auto durable = ReadClockTestFile(path);
    const Luminumbra::Vec3 remote(3208, 408, 3208);
    EXPECT_EQ(world->EditTerrainVoxel(remote, 4.0f, false, nullptr), 0);
    EXPECT_EQ(world->EditTerrainBed(remote, -500, 4.0f), 0);
    EXPECT_EQ(terrain->sdf_data, before);
    EXPECT_EQ(ReadClockTestFile(path), durable);
    EXPECT_TRUE(world->authority_fault());
}

TEST(GameSessionHeadlessWorldTest, IncompleteHeightfieldAuthorityRefusesSurfaceEntry) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    ASSERT_TRUE(session.CreateTransientWorld("Incomplete surface", "1337", "default"));
    auto* world = session.GetWorldSystem();
    const auto spawn = session.GetMetadata().spawnPoint;
    const auto coords = Luminumbra::Systems::SHIELD_WorldSystem::world_to_chunk_coords(
        {spawn.x, world->GetTerrainHeightAt(spawn.x, spawn.z), spawn.z});
    const auto terrain = std::make_shared<Luminumbra::Chunk>(coords);
    terrain->heightmap_data = {8.0f};
    terrain->mark_voxel_data_dirty();
    ASSERT_TRUE(world->adopt_streamed_chunk(terrain));
    EXPECT_FALSE(world->EnsureSurfaceReadyNear(spawn, session.GetPhysicsSystem(), 0, 0));
    EXPECT_EQ(terrain->heightmap_data, std::vector<float>{8.0f});
    EXPECT_TRUE(terrain->sdf_data.empty());
    EXPECT_FALSE(terrain->has_collision.load());
}

TEST(GameSessionHeadlessWorldTest, WaterAuthorityMetadataRefusesUnknownAndPartialValues) {
    Luminumbra::WorldStreamingState state;
    const auto chunk = FlowingRegionChunk({0, 0, 0});
    chunk->mark_sdf_generated_current_params();
    chunk->mark_water_data_dirty();
    state.insert_chunk(chunk);
    const auto snapshot = nlohmann::json::parse(P::SerializeWorldStreamingStateSnapshotJson(state));
    EXPECT_EQ(snapshot.at("chunks").at(0).at("water_authority"), "generated");
    for (const auto& invalid : {nlohmann::json("unknown"),
                                nlohmann::json(nullptr),
                                nlohmann::json(1),
                                nlohmann::json::object()}) {
        auto changed = snapshot;
        changed["chunks"][0]["water_authority"] = invalid;
        Luminumbra::WorldStreamingState loaded;
        std::vector<std::string> errors;
        EXPECT_FALSE(P::LoadWorldStreamingStateSnapshotJson(changed.dump(), loaded, errors));
        EXPECT_FALSE(errors.empty());
        errors.clear();
        EXPECT_FALSE(P::ValidateWorldStreamingChunkFormatJson(changed["chunks"][0].dump(), errors));
    }
}

TEST(GameSessionHeadlessWorldTest, SimulationCollisionPrecedesPhysicsAcrossRenderSchedules) {
    using Luminumbra::Vec3;
    namespace C = Luminumbra::Components;
    const HeadlessRoot root;
    const auto preset_path = root.path() / "worlds/atlas/presets/default.json";
    auto preset = nlohmann::json::parse(ReadClockTestFile(preset_path));
    preset["generation_params"]["terrain"]["height_offset"] = 256.0;
    {
        std::ofstream output(preset_path);
        output << preset.dump();
        ASSERT_TRUE(output.good());
    }
    JobSystem jobs;
    jobs.startup(1);
    GameSession rendered, headless;
    const Vec3 anchor(81928, 512, 81928);
    std::array<entt::entity, 2> creatures{}, outside_creatures{};
    const Vec3 outside(-81928, 512, -81928);
    std::size_t session_index = 0;
    for (auto* session : {&rendered, &headless}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetActiveRegionsEnabled(true);
        session->SetWeatherRainEnabled(false);
        ASSERT_TRUE(session->CreateTransientWorld("Collision schedule", "1337", "default"));
        session->SetReplicatedSimulationAnchors({anchor});
        session->GetWorldSystem()->debug_set_streaming_radius_cap(1);
        const float ground = session->GetWorldSystem()->GetTerrainHeightAt(anchor.x, anchor.z);
        ASSERT_GT(ground, session->GetWorldSystem()->WaterLevelAt(anchor.x, anchor.z));
        const Vec3 feet(anchor.x, ground + 0.25f, anchor.z);
        auto& registry = session->GetRegistry();
        const auto creature = registry.create();
        registry.emplace<C::TransformComponent>(creature).position = feet;
        registry.emplace<C::CreatureComponent>(creature).move_speed = 0.0f;
        registry.emplace<C::CreaturePhysicsComponent>(creature).avatar_index =
            session->GetPhysicsSystem()->create_avatar_character(feet);
        creatures[session_index] = creature;
        const auto outside_creature = registry.create();
        const float outside_ground =
            session->GetWorldSystem()->GetTerrainHeightAt(outside.x, outside.z);
        const Vec3 outside_feet(outside.x, outside_ground + 0.25f, outside.z);
        registry.emplace<C::TransformComponent>(outside_creature).position = outside_feet;
        registry.emplace<C::CreatureComponent>(outside_creature).move_speed = 0.0f;
        registry.emplace<C::CreaturePhysicsComponent>(outside_creature).avatar_index =
            session->GetPhysicsSystem()->create_avatar_character(outside_feet);
        outside_creatures[session_index++] = outside_creature;
    }
    // Only one execution has completed rendering its surface horizon at tick zero.
    ASSERT_TRUE(rendered.GetWorldSystem()->EnsureSurfaceReadyNear(
        anchor, rendered.GetPhysicsSystem(), 0, 0));
    ASSERT_TRUE(rendered.GetWorldSystem()->EnsureSurfaceReadyNear(
        outside, rendered.GetPhysicsSystem(), 0, 0));
    for (int tick = 0; tick < 8; ++tick) {
        ASSERT_EQ(rendered.TickSimulation(rendered.GetSimulationClock().fixed_dt()), 1u);
        ASSERT_EQ(headless.TickSimulation(headless.GetSimulationClock().fixed_dt()), 1u);
        ASSERT_FALSE(rendered.GetRegionSchedulerFault());
        ASSERT_FALSE(headless.GetRegionSchedulerFault());
        EXPECT_EQ(rendered.GetRegistry().get<C::TransformComponent>(creatures[0]).position,
                  headless.GetRegistry().get<C::TransformComponent>(creatures[1]).position);
        EXPECT_EQ(rendered.GetRegistry().get<C::TransformComponent>(outside_creatures[0]).position,
                  headless.GetRegistry().get<C::TransformComponent>(outside_creatures[1]).position);
        EXPECT_EQ(rendered.GetActiveRegionLedger().canonical_bytes(),
                  headless.GetActiveRegionLedger().canonical_bytes());
        EXPECT_EQ(luminumbra::ai::ComputeEcologySubHash(rendered.GetRegistry()),
                  luminumbra::ai::ComputeEcologySubHash(headless.GetRegistry()));
        for (const auto position : {anchor, outside}) {
            const auto feet = headless.GetRegistry()
                                  .get<C::TransformComponent>(
                                      position == anchor ? creatures[1] : outside_creatures[1])
                                  .position;
            const auto terrain = headless.GetWorldSystem()->GetTerrainHeightAt(feet.x, feet.z);
            EXPECT_GT(feet.y, terrain - 0.25f);
            EXPECT_TRUE(headless.GetPhysicsSystem()
                            ->audio_raycast(Vec3(feet.x, terrain + 2.0f, feet.z),
                                            Vec3(feet.x, terrain - 2.0f, feet.z))
                            .hit);
        }
        const float ground = headless.GetWorldSystem()->GetTerrainHeightAt(anchor.x, anchor.z);
        EXPECT_TRUE(headless.GetPhysicsSystem()
                        ->audio_raycast(Vec3(anchor.x, ground + 2.0f, anchor.z),
                                        Vec3(anchor.x, ground - 2.0f, anchor.z))
                        .hit);
        EXPECT_EQ(rendered.GetWorldSystem()->compute_chunk_hashes().world,
                  headless.GetWorldSystem()->compute_chunk_hashes().world);
        for (const auto id : headless.GetWorldSystem()->simulation_chunk_ids()) {
            const auto chunk =
                headless.GetWorldSystem()->find_streamed_chunk(Luminumbra::Chunk::decode_id(id));
            ASSERT_NE(chunk, nullptr);
            EXPECT_TRUE(chunk->has_collision.load());
        }
        if (tick % 2 == 0) {
            rendered.GetWorldSystem()->update(
                rendered.GetRegistry(), anchor, rendered.GetPhysicsSystem());
            rendered.GetWorldSystem()->wait_for_streaming_jobs();
        }
        if (tick % 3 == 0) {
            headless.GetWorldSystem()->update(
                headless.GetRegistry(), Vec3(-81928, 512, -81928), headless.GetPhysicsSystem());
            headless.GetWorldSystem()->wait_for_streaming_jobs();
        }
    }
}

TEST(GameSessionHeadlessWorldTest, NonzeroLegacyWaterContinuesCompleteStateAcrossSaveAndEdits) {
    namespace C = Luminumbra::Components;
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession legacy, continued, resumed;
    for (auto* session : {&legacy, &continued, &resumed}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetWeatherRainEnabled(false);
        session->SetWaterHighResEnabled(true);
    }
    legacy.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(legacy.CreateWorld("Elevated saved water", "1337", "default"));
    auto original = FlowingRegionChunk({10, 7, 10});
    original->mark_sdf_generated_current_params();
    original->water_bed_mm[3] = -700;
    original->water_edge_flux[5] = -41;
    Luminumbra::WorldStreamingState state;
    state.insert_chunk(original);
    P::WorldSaveService service;
    ASSERT_TRUE(service.save_world(state, legacy.GetWorldSaveDir()));
    continued.SetActiveRegionsEnabled(true);
    resumed.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(continued.LoadWorld(legacy.GetMetadata().worldId));
    auto* world = continued.GetWorldSystem();
    const auto old_owner = world->find_streamed_chunk({10, 7, 10});
    ASSERT_NE(old_owner, nullptr);
    // Live same-id changes outrank the older durable image.
    old_owner->water_depth_mm[3] += 31;
    old_owner->mark_water_data_dirty();
    const auto before = old_owner->authority_stamp();
    const auto old_terrain = old_owner->sdf_data;
    const auto cached_before = world->compute_chunk_hashes().world;
    auto render_cache = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(10, 0, 10));
    world->GenerateChunkData(*render_cache);
    ASSERT_FALSE(render_cache->has_water_authority());
    ASSERT_TRUE(world->adopt_streamed_chunk(render_cache));
    world->prepare_region_authority(continued.GetRegistry(), {{0, 0}}, {}, nullptr);
    ASSERT_FALSE(world->authority_fault());
    const auto column = world->find_streamed_chunk({10, 0, 10});
    ASSERT_NE(column, nullptr);
    EXPECT_EQ(column->water_depth_mm[3], original->water_depth_mm[3] + 31);
    EXPECT_EQ(column->water_bed_mm, original->water_bed_mm);
    EXPECT_EQ(column->water_edge_flux, original->water_edge_flux);
    EXPECT_EQ(column->authority_stamp().water_resolution, before.water_resolution);
    EXPECT_EQ(column->authority_stamp().quiet_ticks, before.quiet_ticks);
    EXPECT_EQ(column->authority_stamp().max_delta_bits, before.max_delta_bits);
    EXPECT_FALSE(old_owner->has_water_authority());
    EXPECT_FALSE(old_owner->has_water_sim.load());
    EXPECT_TRUE(old_owner->water_depth_mm.empty());
    EXPECT_EQ(old_owner->sdf_data, old_terrain);
    EXPECT_NE(world->compute_chunk_hashes().world, cached_before);
    EXPECT_EQ(world->compute_chunk_hashes().world,
              P::BindWaterScheduler(
                  P::WorldSaveService::authority_hashes(world->authority_directory(), {}),
                  world->GetWaterSimWindowCursor())
                  .world);
    ASSERT_TRUE(resumed.CreateWorld("Resumed elevated water", "1337", "default"));
    const auto snapshot = root.path() / "migrated-water";
    ASSERT_TRUE(continued.SaveWorldStateTo(snapshot, nullptr, true));
    ASSERT_TRUE(resumed.LoadWorldStateFrom(snapshot));
    const auto restored = resumed.GetWorldSystem()->find_streamed_chunk({10, 0, 10});
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->water_depth_mm, column->water_depth_mm);
    EXPECT_EQ(restored->water_bed_mm, column->water_bed_mm);
    EXPECT_EQ(restored->water_edge_flux, column->water_edge_flux);
    const Luminumbra::Vec3 position(168, 120, 168);
    for (auto* session : {&continued, &resumed}) {
        session->SetReplicatedSimulationAnchors({position});
        EXPECT_GT(session->GetWorldSystem()->EditTerrainBed(position, -200, 4.0f), 0);
        const auto entity = session->GetRegistry().create();
        session->GetRegistry().emplace<C::TransformComponent>(entity).position = position;
        session->GetRegistry().emplace<C::FlowingWaterSourceComponent>(entity).flow_rate = 4.0f;
    }
    const auto depth_before_tick = column->water_depth_mm;
    const auto flux_before_tick = column->water_edge_flux;
    for (int tick = 0; tick < 4; ++tick) {
        for (auto* session : {&continued, &resumed}) {
            ASSERT_EQ(session->TickSimulation(session->GetSimulationClock().fixed_dt()), 1u);
            ASSERT_FALSE(session->GetRegionSchedulerFault());
        }
        EXPECT_EQ(world->compute_chunk_hashes().world,
                  resumed.GetWorldSystem()->compute_chunk_hashes().world);
        EXPECT_EQ(world->simulation_chunk_ids(), resumed.GetWorldSystem()->simulation_chunk_ids());
        EXPECT_EQ(continued.GetActiveRegionLedger().canonical_bytes(),
                  resumed.GetActiveRegionLedger().canonical_bytes());
    }
    EXPECT_NE(column->water_depth_mm, depth_before_tick);
    EXPECT_NE(column->water_edge_flux, flux_before_tick);
    EXPECT_NE(column->water_bed_mm, original->water_bed_mm);
    EXPECT_TRUE(old_owner->water_depth_mm.empty());
    const auto retired = resumed.GetWorldSystem()->find_streamed_chunk({10, 7, 10});
    EXPECT_TRUE(!retired || retired->water_depth_mm.empty());
    EXPECT_GT(world->debug_water_sources_seen(), 0u);
}

TEST(GameSessionHeadlessWorldTest, EqualWaterColumnDuplicatesCoalesceIndependentOfHistory) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession forward, reverse;
    for (auto* session : {&forward, &reverse}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetActiveRegionsEnabled(true);
        ASSERT_TRUE(session->CreateWorld("Equal column copies", "1337", "default"));
        const std::array<int, 3> layers =
            session == &forward ? std::array{0, -3, 7} : std::array{7, -3, 0};
        for (const int layer : layers) {
            auto chunk = FlowingRegionChunk({10, layer, 10});
            chunk->mark_sdf_generated_current_params();
            chunk->mark_water_data_dirty();
            ASSERT_TRUE(session->GetWorldSystem()->adopt_streamed_chunk(chunk));
        }
        session->GetWorldSystem()->prepare_region_authority(
            session->GetRegistry(), {{0, 0}}, {}, nullptr);
        ASSERT_FALSE(session->GetWorldSystem()->authority_fault());
        const auto canonical = session->GetWorldSystem()->find_streamed_chunk({10, 0, 10});
        ASSERT_NE(canonical, nullptr);
        EXPECT_EQ(canonical->water_depth_mm, FlowingRegionChunk({10, 0, 10})->water_depth_mm);
        EXPECT_EQ(session->GetWorldSystem()->simulation_chunk_ids(),
                  std::vector<Luminumbra::ChunkID>{canonical->get_id()});
        for (const int layer : {-3, 7}) {
            const auto retired = session->GetWorldSystem()->find_streamed_chunk({10, layer, 10});
            EXPECT_TRUE(!retired || !retired->has_water_sim.load());
        }
        ASSERT_TRUE(session->SaveWorldState());
        ASSERT_TRUE(session->LoadWorldState());
        session->GetWorldSystem()->prepare_region_authority(
            session->GetRegistry(), {{0, 0}}, {}, nullptr);
    }
    EXPECT_EQ(forward.GetWorldSystem()->compute_chunk_hashes().world,
              reverse.GetWorldSystem()->compute_chunk_hashes().world);
}

TEST(GameSessionHeadlessWorldTest, ConflictingOrIncompleteColumnWaterRefusesWithoutWrites) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    // Each mutation changes a persisted input consumed by the migration.
    for (int mutation = 0; mutation < 17; ++mutation) {
        SCOPED_TRACE(mutation);
        GameSession session;
        session.SetRootPath(root.root_string());
        session.SetJobSystem(&jobs);
        session.SetActiveRegionsEnabled(true);
        ASSERT_TRUE(session.CreateWorld("Ambiguous water", "1337", "default"));
        auto* world = session.GetWorldSystem();
        auto source = FlowingRegionChunk({10, 7, 10});
        auto other = FlowingRegionChunk({10, mutation % 2 == 0 ? 0 : -3, 10});
        source->mark_water_data_dirty();
        other->mark_water_data_dirty();
        switch (mutation) {
            case 0:
                ++other->water_depth_mm[0];
                break;
            case 1:
                ++other->water_bed_mm[0];
                break;
            case 2:
                ++other->water_edge_flux[0];
                break;
            case 3:
                other->current_water_resolution.store(4);
                break;
            case 4:
                other->is_water_sleeping.store(true);
                break;
            case 5:
                ++other->ticks_below_threshold;
                break;
            case 6:
                other->max_water_delta_last_tick += 1.0f;
                break;
            case 7:
                other->has_water_sim.store(false);
                break;
            case 8:
                other->water_edge_flux.pop_back();
                break;
            case 9:
                other->current_water_resolution.store(0);
                break;
            case 10:
                other->water_depth_mm[0] = -1;
                break;
            case 11:
                other->water_level_data.pop_back();
                break;
            case 12:
                other->water_flow_data[0].x = std::numeric_limits<float>::infinity();
                break;
            case 13:
                other->water_bed_mm.clear();
                break;
            case 14:
                other->water_level_data[0] += 1.0f;
                break;
            case 15:
                other->water_flow_data.clear();
                break;
            case 16:
                other->water_sim_terrain_height[0] += 1.0f;
                break;
            default:
                FAIL();
        }
        ASSERT_TRUE(world->adopt_streamed_chunk(source));
        ASSERT_TRUE(world->adopt_streamed_chunk(other));
        ASSERT_TRUE(session.SaveWorldState());
        const auto path = P::WorldSaveService::region_file_path(world->authority_directory(), 0, 0);
        const auto bytes = ReadClockTestFile(path);
        const auto first = P::SerializeChunkAuthoritySnapshotJson(*source);
        const auto second = P::SerializeChunkAuthoritySnapshotJson(*other);
        EXPECT_THROW(world->prepare_region_authority(session.GetRegistry(), {{0, 0}}, {}, nullptr),
                     std::runtime_error);
        EXPECT_TRUE(world->authority_fault());
        EXPECT_EQ(ReadClockTestFile(path), bytes);
        EXPECT_EQ(P::SerializeChunkAuthoritySnapshotJson(*source), first);
        EXPECT_EQ(P::SerializeChunkAuthoritySnapshotJson(*other), second);
    }
}

TEST(GameSessionHeadlessWorldTest, ColumnWaterReplacementFailurePreservesOwnersAndDurableImage) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    for (const bool partial : {false, true}) {
        SCOPED_TRACE(partial);
        GameSession session;
        session.SetRootPath(root.root_string());
        session.SetJobSystem(&jobs);
        ASSERT_TRUE(session.CreateWorld("Interrupted migration", "1337", "default"));
        auto* world = session.GetWorldSystem();
        auto source = FlowingRegionChunk({10, 7, 10});
        source->mark_water_data_dirty();
        ASSERT_TRUE(world->adopt_streamed_chunk(source));
        ASSERT_TRUE(session.SaveWorldState());
        const auto path = P::WorldSaveService::region_file_path(world->authority_directory(), 0, 0);
        if (partial)
            std::ofstream(path, std::ios::binary) << "partial";
        const auto bytes = ReadClockTestFile(path);
        const auto before = P::SerializeChunkAuthoritySnapshotJson(*source);
        P::WorldSaveService::set_interrupt_before_region_replace_for_testing(!partial);
        EXPECT_THROW(world->prepare_region_authority(session.GetRegistry(), {{0, 0}}, {}, nullptr),
                     std::runtime_error);
        P::WorldSaveService::set_interrupt_before_region_replace_for_testing(false);
        EXPECT_TRUE(world->authority_fault());
        EXPECT_EQ(world->find_streamed_chunk({10, 7, 10}), source);
        EXPECT_EQ(world->find_streamed_chunk({10, 0, 10}), nullptr);
        EXPECT_EQ(P::SerializeChunkAuthoritySnapshotJson(*source), before);
        EXPECT_EQ(ReadClockTestFile(path), bytes);
    }
}

TEST(GameSessionHeadlessWorldTest, PristineTerrainParticipantsForgetDistantHorizonsAcrossReload) {
    const HeadlessRoot root;
    const auto preset_path = root.path() / "worlds/atlas/presets/default.json";
    auto preset = nlohmann::json::parse(ReadClockTestFile(preset_path));
    preset["generation_params"]["terrain"]["height_offset"] = 256.0;
    std::ofstream(preset_path) << preset.dump();
    JobSystem jobs;
    jobs.startup(1);
    for (const bool over_budget : {false, true}) {
        SCOPED_TRACE(over_budget);
        GameSession uninterrupted, loaded;
        for (auto* session : {&uninterrupted, &loaded}) {
            session->SetRootPath(root.root_string());
            session->SetJobSystem(&jobs);
            session->SetActiveRegionsEnabled(true);
            session->SetWeatherRainEnabled(false);
            session->SetRegionSchedulerConfig({1000000000, 3, 1});
        }
        ASSERT_TRUE(uninterrupted.CreateWorld("Horizon membership", "1337", "default"));
        const Luminumbra::Vec3 first(136, 300, 136), second(392, 300, 392);
        const Luminumbra::world::RegionKey remote{20, 20};
        uninterrupted.PinActiveRegion(remote, true);
        uninterrupted.SetReplicatedSimulationAnchors({first});
        ASSERT_EQ(uninterrupted.TickSimulation(uninterrupted.GetSimulationClock().fixed_dt()), 1u);
        auto* world = uninterrupted.GetWorldSystem();
        const float ground = world->GetTerrainHeightAt(first.x, first.z);
        const auto old_coords = Luminumbra::Systems::SHIELD_WorldSystem::world_to_chunk_coords(
            {first.x, ground, first.z});
        ASSERT_NE(old_coords.y, 0);
        const auto old_chunk = world->find_streamed_chunk(old_coords);
        ASSERT_NE(old_chunk, nullptr);
        ASSERT_FALSE(old_chunk->has_simulation_authority());
        ASSERT_TRUE(old_chunk->has_collision.load());
        uninterrupted.SetReplicatedSimulationAnchors({second});
        if (over_budget)
            uninterrupted.RecordActiveRegionWork({remote, 1000000001, 0, 0, 0});
        ASSERT_EQ(uninterrupted.TickSimulation(uninterrupted.GetSimulationClock().fixed_dt()), 1u);
        ASSERT_FALSE(uninterrupted.GetRegionSchedulerFault());
        const auto record = uninterrupted.GetActiveRegionLedger().records().at(remote);
        EXPECT_GT(over_budget ? record.over_hold : record.under_hold, 0u);
        const auto save = root.path() / (over_budget ? "over-hold-horizon" : "under-hold-horizon");
        ASSERT_TRUE(uninterrupted.SaveWorldStateTo(save));
        ASSERT_TRUE(loaded.CreateWorld("Restored horizon membership", "1337", "default"));
        ASSERT_TRUE(loaded.LoadWorldStateFrom(save));
        loaded.SetReplicatedSimulationAnchors({second});
        // Same position and absolute tick, before either continuation advances.
        uninterrupted.PrepareSimulationCollisions();
        loaded.PrepareSimulationCollisions();
        EXPECT_EQ(uninterrupted.GetSimulationTickCount(), loaded.GetSimulationTickCount());
        // These sorted ids are the complete active-region input to the server
        // availability digest; equality is stronger than comparing its hash.
        EXPECT_EQ(world->simulation_chunk_ids(), loaded.GetWorldSystem()->simulation_chunk_ids());
        EXPECT_FALSE(old_chunk->has_collision.load());
        const auto restored = loaded.GetWorldSystem()->find_streamed_chunk(old_coords);
        if (restored) {
            EXPECT_FALSE(restored->has_collision.load());
        }
        // Isolate the departed terrain layer from its Y=0 column's heightfield.
        const auto old_water = Luminumbra::Chunk::calculate_id({old_coords.x, 0, old_coords.z});
        for (auto* session : {&uninterrupted, &loaded}) {
            session->GetPhysicsSystem()->remove_chunk_collision(old_water);
            EXPECT_FALSE(session->GetPhysicsSystem()
                             ->audio_raycast({first.x, ground + 2.0f, first.z},
                                             {first.x, ground - 2.0f, first.z})
                             .hit);
        }
        const auto loaded_record = loaded.GetActiveRegionLedger().records().at(remote);
        EXPECT_EQ(loaded_record.over_hold, record.over_hold);
        EXPECT_EQ(loaded_record.under_hold, record.under_hold);
        EXPECT_EQ(uninterrupted.GetActiveRegionLedger().canonical_bytes(),
                  loaded.GetActiveRegionLedger().canonical_bytes());
    }
}

TEST(GameSessionHeadlessWorldTest, EvictionPreparationRequiresAdmissionPressure) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    ASSERT_TRUE(session.CreateTransientWorld("Pressure-driven eviction", "1337", "default"));
    auto* world = session.GetWorldSystem();
    const Luminumbra::Vec3 first(136, 0, 136), second(392, 0, 392);
    world->prepare_region_authority(session.GetRegistry(), {{0, 0}}, {first}, nullptr);
    const auto baseline = world->authority_eviction_preparations();
    std::vector<Luminumbra::ChunkID> caches;
    const auto headroom = world->kActiveChunkBudget - world->snapshot_streamed_chunks().size();
    for (std::size_t index = 0; index < headroom; ++index) {
        auto chunk = std::make_shared<Luminumbra::Chunk>(
            Luminumbra::IVec3(1000 + static_cast<int>(index), 2, 1000));
        caches.push_back(chunk->get_id());
        ASSERT_TRUE(world->adopt_streamed_chunk(chunk));
    }
    ASSERT_EQ(world->snapshot_streamed_chunks().size(), world->kActiveChunkBudget);
    // Full residency alone is not pressure: no allocation/admission is requested.
    for (int tick = 0; tick < 3; ++tick)
        world->prepare_region_authority(session.GetRegistry(), {{0, 0}}, {first}, nullptr);
    EXPECT_EQ(world->authority_eviction_preparations(), baseline);
    world->prepare_region_authority(session.GetRegistry(), {{0, 0}}, {first, second}, nullptr);
    ASSERT_FALSE(world->authority_fault());
    EXPECT_EQ(world->authority_eviction_preparations(), baseline + 1);
    EXPECT_EQ(world->snapshot_streamed_chunks().size(), world->kActiveChunkBudget);
    std::sort(caches.begin(), caches.end());
    EXPECT_EQ(world->find_streamed_chunk(Luminumbra::Chunk::decode_id(caches.front())), nullptr);
    EXPECT_NE(world->find_streamed_chunk(Luminumbra::Chunk::decode_id(caches.back())), nullptr);
}

TEST(GameSessionHeadlessWorldTest, ColumnWaterMigrationAtCapacityRetiresOnlyWaterOnlySources) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    for (const bool terrain_owned : {false, true}) {
        SCOPED_TRACE(terrain_owned);
        GameSession session;
        session.SetRootPath(root.root_string());
        session.SetJobSystem(&jobs);
        session.SetActiveRegionsEnabled(true);
        ASSERT_TRUE(session.CreateWorld("Full column migration", "1337", "default"));
        auto* world = session.GetWorldSystem();
        auto source = FlowingRegionChunk({10, 7, 10});
        if (!terrain_owned)
            source->mark_sdf_generated_current_params();
        source->mark_water_data_dirty();
        ASSERT_TRUE(world->adopt_streamed_chunk(source));
        // Fill the shared budget with independent terrain authority in this region.
        // Empty terrain payloads suffice: membership is the capacity constraint.
        for (std::size_t index = 1; index < world->kActiveChunkBudget; ++index) {
            const auto offset = static_cast<int>(index);
            auto chunk = std::make_shared<Luminumbra::Chunk>(
                Luminumbra::IVec3(offset % 32, 100 + offset / 1024, (offset / 32) % 32));
            chunk->mark_sdf_loaded_or_edited();
            ASSERT_TRUE(world->adopt_streamed_chunk(chunk));
        }
        ASSERT_EQ(world->snapshot_streamed_chunks().size(), world->kActiveChunkBudget);
        const auto before = P::SerializeChunkAuthoritySnapshotJson(*source);
        const auto depth = source->water_depth_mm;
        const auto flux = source->water_edge_flux;
        const auto path = P::WorldSaveService::region_file_path(world->authority_directory(), 0, 0);
        EXPECT_FALSE(std::filesystem::exists(path));
        if (terrain_owned) {
            EXPECT_THROW(
                world->prepare_region_authority(session.GetRegistry(), {{0, 0}}, {}, nullptr),
                std::runtime_error);
            EXPECT_TRUE(world->authority_fault());
            EXPECT_EQ(P::SerializeChunkAuthoritySnapshotJson(*source), before);
            EXPECT_EQ(world->find_streamed_chunk({10, 0, 10}), nullptr);
            EXPECT_FALSE(std::filesystem::exists(path));
        } else {
            world->prepare_region_authority(session.GetRegistry(), {{0, 0}}, {}, nullptr);
            ASSERT_FALSE(world->authority_fault());
            const auto canonical = world->find_streamed_chunk({10, 0, 10});
            ASSERT_NE(canonical, nullptr);
            EXPECT_EQ(canonical->water_depth_mm, depth);
            EXPECT_EQ(canonical->water_edge_flux, flux);
            EXPECT_EQ(world->find_streamed_chunk({10, 7, 10}), nullptr);
            EXPECT_EQ(world->authority_eviction_preparations(), 0u);
            P::WorldSaveService::AuthorityReader reader;
            ASSERT_TRUE(reader.open(path));
            const auto durable = reader.read(canonical->get_id());
            ASSERT_NE(durable, nullptr);
            EXPECT_EQ(durable->water_depth_mm, depth);
            EXPECT_EQ(durable->water_edge_flux, flux);
        }
        EXPECT_EQ(world->snapshot_streamed_chunks().size(), world->kActiveChunkBudget);
    }
}

#include "../support/WaterSchedulerFixture.h"

TEST(GameSessionHeadlessWorldTest, AuthorityCaptureRequiresACompletedWorldBoundary) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    for (const bool regions : {false, true}) {
        GameSession session;
        std::string captured_world;
        EXPECT_THROW(captured_world = session.CaptureAuthorityHashes().world, std::logic_error);
        session.SetRootPath(root.root_string());
        session.SetJobSystem(&jobs);
        session.SetActiveRegionsEnabled(regions);
        ASSERT_TRUE(session.CreateTransientWorld("Authority boundary", "1337", "default"));
        bool observed = false;
        session.GetSimulationEventBus().subscribe(
            [&](const luminumbra::simulation::SimulationEvent&) {
                observed = true;
                EXPECT_EQ(session.IsSimulationTickBoundary(), !regions);
                EXPECT_THROW(captured_world = session.CaptureAuthorityHashes().world,
                             std::logic_error);
            });
        session.GetSimulationEventBus().publish(1, "capture", "");
        ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
        ASSERT_TRUE(observed);
        ASSERT_TRUE(session.IsSimulationTickBoundary());
        EXPECT_TRUE(captured_world.empty());
        EXPECT_NO_THROW(captured_world = session.CaptureAuthorityHashes().world);
        EXPECT_FALSE(captured_world.empty());
    }
}

TEST(GameSessionHeadlessWorldTest, NonzeroSchedulerRoundTripsBothLoadPathsAndChangesContinuation) {
    for (const bool regions : {false, true}) {
        for (const auto& [resolution, count] :
             {std::pair{8, std::size_t{65}}, std::pair{16, std::size_t{17}}}) {
            const HeadlessRoot root;
            JobSystem jobs;
            jobs.startup(1);
            GameSession original, fresh, changed;
            for (auto* session : {&original, &fresh, &changed}) {
                session->SetRootPath(root.root_string());
                session->SetJobSystem(&jobs);
                session->SetActiveRegionsEnabled(regions);
                session->SetWaterHighResEnabled(resolution == 16);
                session->SetWeatherRainEnabled(false);
                session->SetWeatherEventsEnabled(false);
                session->SetFiniteHydrologyEnabled(true);
            }
            ASSERT_TRUE(original.CreateWorld("Scheduler continuation", "1337", "default"));
            auto original_chunks =
                Luminumbra::TestSupport::InstallSchedulerChunks(original, resolution, count);
            original.GetWorldSystem()->SetWaterSimWindowCursor(count - 1);
            const auto clock = original.GetWorldClock().canonical_bytes();
            const auto ledger = original.GetActiveRegionLedger().canonical_bytes();
            const auto identity = original.SystemConfigIdentity();
            const auto before = original.CaptureAuthorityHashes();
            ASSERT_TRUE(original.SaveWorldState(nullptr, true));
            ASSERT_TRUE(original.SaveWorld());
            const auto save = original.GetWorldSaveDir();
            const auto metadata_path = save / "world_info.json";
            const auto metadata_bytes = ReadClockTestFile(metadata_path);
            ASSERT_TRUE(fresh.LoadWorld(original.GetMetadata().worldId));
            EXPECT_EQ(fresh.GetWorldSystem()->GetWaterSimWindowCursor(), count - 1);
            EXPECT_EQ(fresh.CaptureAuthorityHashes().sub.water, before.sub.water);
            EXPECT_EQ(fresh.CaptureAuthorityHashes().world, before.world);
            EXPECT_EQ(fresh.SystemConfigIdentity(), identity);
            EXPECT_EQ(fresh.GetWorldClock().canonical_bytes(), clock);
            EXPECT_EQ(fresh.GetActiveRegionLedger().canonical_bytes(), ledger);
            original.GetWorldSystem()->SetWaterSimWindowCursor(0);
            ASSERT_TRUE(original.LoadWorldStateFrom(save));
            EXPECT_EQ(original.GetWorldSystem()->GetWaterSimWindowCursor(), count - 1);
            EXPECT_EQ(original.CaptureAuthorityHashes().sub.water, before.sub.water);
            EXPECT_EQ(original.CaptureAuthorityHashes().world, before.world);
            auto metadata = nlohmann::json::parse(metadata_bytes);
            metadata["waterSimCursor"] = 0u;
            WriteSavedPresetTestJson(metadata_path, metadata);
            ASSERT_TRUE(changed.LoadWorld(original.GetMetadata().worldId));
            EXPECT_EQ(changed.GetWorldSystem()->GetWaterSimWindowCursor(), 0u);
            EXPECT_EQ(changed.SystemConfigIdentity(), identity);
            EXPECT_EQ(changed.GetWorldClock().canonical_bytes(), clock);
            EXPECT_EQ(changed.GetActiveRegionLedger().canonical_bytes(), ledger);
            EXPECT_EQ(changed.CaptureAuthorityHashes().sub.terrain, before.sub.terrain);
            EXPECT_NE(changed.CaptureAuthorityHashes().sub.water, before.sub.water);
            EXPECT_NE(changed.CaptureAuthorityHashes().world, before.world);
            const auto chunks_for = [](GameSession& session) {
                Luminumbra::TestSupport::SchedulerChunks chunks;
                for (const auto& chunk : session.GetWorldSystem()->snapshot_streamed_chunks())
                    chunks.emplace(chunk->get_id(), chunk);
                return chunks;
            };
            original_chunks = chunks_for(original);
            auto fresh_chunks = chunks_for(fresh);
            auto changed_chunks = chunks_for(changed);
            for (int step = 0; step < 3; ++step) {
                for (const auto& pair : {std::pair{&original, &original_chunks},
                                         std::pair{&fresh, &fresh_chunks},
                                         std::pair{&changed, &changed_chunks}}) {
                    pair.first->GetWaterSystem()->SetBootPaused(false);
                    pair.first->GetWaterSystem()->SetHydrology(true, 0, 0);
                    pair.first->GetWaterSystem()->update(
                        pair.first->GetRegistry(), *pair.second, regions);
                }
                EXPECT_EQ(original.CaptureAuthorityHashes().world,
                          fresh.CaptureAuthorityHashes().world);
                EXPECT_EQ(original.GetWorldSystem()->GetWaterSimWindowCursor(),
                          fresh.GetWorldSystem()->GetWaterSimWindowCursor());
                if (step == 0) {
                    std::size_t different = 0;
                    for (const auto& [id, chunk] : original_chunks)
                        different += chunk->water_depth_mm != changed_chunks.at(id)->water_depth_mm;
                    EXPECT_GT(different, 0u);
                }
            }
            // Reading or selecting the current projection never rewrites saved metadata.
            EXPECT_EQ(nlohmann::json::parse(ReadClockTestFile(metadata_path)), metadata);
        }
    }
}

TEST(GameSessionHeadlessWorldTest,
     DisabledRegionsIncrementalSaveRestoresSchedulerAndAuthorityHash) {
    const HeadlessRoot root;
    JobSystem jobs;
    jobs.startup(1);
    GameSession original, loaded;
    for (auto* session : {&original, &loaded}) {
        session->SetRootPath(root.root_string());
        session->SetJobSystem(&jobs);
        session->SetActiveRegionsEnabled(false);
        session->SetWaterHighResEnabled(false);
        session->SetWeatherRainEnabled(false);
        session->SetWeatherEventsEnabled(false);
        session->SetFiniteHydrologyEnabled(true);
    }
    ASSERT_TRUE(original.CreateWorld("Incremental scheduler", "1337", "default"));
    auto chunks = Luminumbra::TestSupport::InstallSchedulerChunks(original, 8, 65);
    ASSERT_TRUE(original.SaveWorldState(nullptr, true));
    const auto save = original.GetWorldSaveDir();
    const auto metadata_path = save / "world_info.json";
    const auto old_metadata = nlohmann::json::parse(ReadClockTestFile(metadata_path));
    ASSERT_EQ(old_metadata.at("waterSimCursor"), 0u);
    original.GetWaterSystem()->SetBootPaused(false);
    original.GetWaterSystem()->SetHydrology(true, 0, 0);
    original.GetWaterSystem()->update(original.GetRegistry(), chunks, false);
    const auto cursor = original.GetWorldSystem()->GetWaterSimWindowCursor();
    ASSERT_NE(cursor, 0u);
    const auto before = original.CaptureAuthorityHashes();
    Luminumbra::world::WorldStateSaveReport report;
    ASSERT_TRUE(original.SaveWorldState(&report));
    EXPECT_TRUE(report.saved);
    EXPECT_GT(report.chunks_dirty, 0u);
    auto expected_metadata = old_metadata;
    expected_metadata["waterSimCursor"] = cursor;
    EXPECT_EQ(nlohmann::json::parse(ReadClockTestFile(metadata_path)), expected_metadata);
    ASSERT_TRUE(loaded.LoadWorld(original.GetMetadata().worldId));
    EXPECT_EQ(loaded.GetWorldSystem()->GetWaterSimWindowCursor(), cursor);
    EXPECT_EQ(loaded.CaptureAuthorityHashes().world, before.world);
    EXPECT_EQ(loaded.CaptureAuthorityHashes().sub.water, before.sub.water);
    original.GetWorldSystem()->SetWaterSimWindowCursor(0);
    ASSERT_TRUE(original.LoadWorldStateFrom(save));
    EXPECT_EQ(original.GetWorldSystem()->GetWaterSimWindowCursor(), cursor);
    EXPECT_EQ(original.CaptureAuthorityHashes().world, before.world);
    // A cursor-only change also survives a save with no dirty chunks, including
    // a return to zero after the metadata previously held a nonzero cursor.
    original.GetWorldSystem()->SetWaterSimWindowCursor(0);
    const auto at_zero = original.CaptureAuthorityHashes();
    ASSERT_TRUE(original.SaveWorldState(&report));
    EXPECT_FALSE(report.saved);
    ASSERT_TRUE(loaded.LoadWorldStateFrom(save));
    EXPECT_EQ(loaded.GetWorldSystem()->GetWaterSimWindowCursor(), 0u);
    EXPECT_EQ(loaded.CaptureAuthorityHashes().world, at_zero.world);
}
