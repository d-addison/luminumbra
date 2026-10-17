// asset-manifest split. The engine-side GameSession validates
// SIMULATION requirements only (preset and worldgen dependencies); renderer/UI
// asset requirements are caller-supplied by the client. Worldgen dependencies
// are simulation assets and must be available even for a headless host.
#include "../support/UniqueTempPath.h"
#include "../support/WorldgenTestData.h"

#include "../support/SystemConfigVariants.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "luminumbra_client/rendering/TimeOfDayModel.h"
#include "luminumbra_common/ai/CircadianSystem.h"
#include "luminumbra_common/ai/CreatureSpeciesRegistry.h"
#include "luminumbra_common/ai/EcologyHash.h"
#include "luminumbra_common/ai/MigrationSystem.h"
#include "luminumbra_common/ai/StimulusChannels.h"
#include "luminumbra_common/components/InstinctComponents.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/net/LockstepSession.h"
#include "luminumbra_common/persistence/SavedWorldCatalog.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/replay/RegionScheduleTrace.h"
#include "luminumbra_common/replay/ReplayStream.h"
#include "luminumbra_common/systems/AetherFieldSystem.h"
#include "luminumbra_common/systems/PlantGrowthSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"

#include <nlohmann/json.hpp>

#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/fields/EnergyFieldState.h"
#include "luminumbra_common/net/LockstepSession.h"
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

// Fault injection exercises scan errors on every platform and under elevated users.
// No permission-dependent fixture or skipped assertion is needed.
enum class SpeciesScanFailure {
    Open,
    Metadata,
    Increment
};

template<SpeciesScanFailure Failure>
class FailingSpeciesDirectoryIterator {
public:
    FailingSpeciesDirectoryIterator() = default;
    FailingSpeciesDirectoryIterator(const fs::path& dir, std::error_code& ec) {
        entry_.file = dir / "deer.json";
        if constexpr (Failure == SpeciesScanFailure::Open) {
            ec = std::make_error_code(std::errc::permission_denied);
        } else {
            active_ = true;
            ec.clear();
        }
    }
    struct Entry {
        fs::path file;
        bool is_regular_file(std::error_code& ec) const {
            if constexpr (Failure == SpeciesScanFailure::Metadata)
                ec = std::make_error_code(std::errc::io_error);
            else
                ec.clear();
            return !ec;
        }
        const fs::path& path() const {
            return file;
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
    const auto baseline = fixture.Resolve();
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
        EXPECT_EQ(resolved.at("schema_rev"), 7);
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
        const auto identity = Luminumbra::world::LoadTerrainPresetFromJson(j, root.path() / "data");
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
        P::WorldSaveService service;
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
    std::ifstream input(path, std::ios::binary);
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

struct PendingSnapshotState;

class SavedWorldPresetTest : public testing::TestWithParam<bool> {
protected:
    HeadlessRoot root;
    JobSystem jobs;

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
    ASSERT_TRUE(fs::remove(root.path() / "worlds/atlas/presets/default.json"));
    const auto before = SavedPresetTestDiskBytes(root.path());
    const auto inspected = P::InspectSavedWorld(root.path(), created.GetMetadata().worldId);
    ASSERT_TRUE(inspected.error.empty()) << inspected.error;
    EXPECT_EQ(inspected.preset_path, save / "preset.json");
    GameSession loaded;
    Configure(loaded);
    ASSERT_TRUE(loaded.LoadWorld(created.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_EQ(loaded.PresetContentHash(), created.PresetContentHash());
    EXPECT_EQ(SavedPresetTestDiskBytes(root.path()), before);
    ASSERT_TRUE(loaded.SaveWorld());
    EXPECT_EQ(nlohmann::json::parse(ReadClockTestFile(save / "world_info.json")), metadata);
}

TEST_P(SavedWorldPresetTest, CustomAdditionalNamedPresetCarriesItsExactIdentity) {
    const auto named = root.path() / "worlds/atlas/presets/other.json";
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

struct PendingSnapshotState {
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
    EXPECT_EQ(loaded.ComputePlantSubHash(), pending.plant_hash);
    ASSERT_NE(loaded.GetEnergyFieldState(), nullptr);
    EXPECT_EQ(loaded.GetEnergyFieldState()->CanonicalBytes(), pending.energy_bytes);
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
    fs::create_directories(root.path() / "worlds/atlas/presets-rev6");
    fs::copy_file(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets-rev6/default.json",
                  root.path() / "worlds/atlas/presets-rev6/default.json");
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
    const auto historical = root.path() / "worlds/atlas/presets-rev6/default.json";
    fs::create_directories(historical.parent_path());
    fs::copy_file(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets-rev6/default.json",
                  historical);
    const auto save = root.path() / "worlds/saves/legacy";
    fs::create_directories(save);
    WriteSavedPresetTestJson(save / "world_info.json",
                             {{"container_version", 2}, {"worldType", "default"}});
    const auto before = SavedPresetTestDiskBytes(root.path());
    const auto inspected = P::InspectSavedWorld(root.path(), "legacy");
    ASSERT_TRUE(inspected.error.empty()) << inspected.error;
    EXPECT_EQ(inspected.preset_path, historical);
    GameSession loaded;
    Configure(loaded);
    ASSERT_TRUE(loaded.LoadWorld("legacy")) << loaded.GetWorldOpenError();
    EXPECT_FALSE(loaded.GetWorldSystem()->get_params().island_mask_enabled);
    EXPECT_EQ(SavedPresetTestDiskBytes(root.path()), before);
    ASSERT_TRUE(loaded.SaveWorld());
    const auto metadata = nlohmann::json::parse(ReadClockTestFile(save / "world_info.json"));
    EXPECT_EQ(metadata.at("presetFormat"), 1);
    EXPECT_EQ(metadata.at("presetIdentity"),
              P::StableChecksum(nlohmann::json::parse(ReadClockTestFile(historical)).dump()));
    EXPECT_EQ(nlohmann::json::parse(ReadClockTestFile(save / "preset.json")),
              nlohmann::json::parse(ReadClockTestFile(historical)));
    ASSERT_TRUE(fs::remove(historical));
    EXPECT_TRUE(P::InspectSavedWorld(root.path(), "legacy").error.empty());
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

// Reads a record that production writes through a text-mode stream, so its bytes
// carry the host's line endings: the energy record (GameSession writes
// aether_state.efs with a default-mode ofstream) is LF on Linux and CRLF on
// Windows. That platform difference predates this change; normalising here keeps
// the assertions about record CONTENT independent of it. Tests that assert exact
// bytes written by the test itself use binary streams instead.
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
    return {{"chunk", P::ComputeWorldStreamingStateHash(state)},
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
    std::ofstream(save / "world_info.json", std::ios::binary | std::ios::trunc) << imported_bytes;
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
    for (auto* session : sessions)
        creatures.push_back(ClockTestCreature(session->GetRegistry()));
    for (int i = 0; i < k; ++i) {
        for (auto* session : sessions)
            ASSERT_EQ(session->TickSimulation(dt), 1u);
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
            for (std::size_t need = 0; need < needs.size(); ++need)
                EXPECT_FLOAT_EQ(needs[need].pressure, expected_needs[need].pressure);
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

    // A failure between the two rewind replacements still leaves a readable pair.
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
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    P::WorldSaveService::set_before_active_regions_replace_for_testing([] { return false; });
    const bool saved = session.SaveWorld();
    P::WorldSaveService::set_before_active_regions_replace_for_testing(nullptr);
    ASSERT_FALSE(saved);
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
    const auto save = fs::directory_iterator(root.path() / "worlds/saves")->path();
    std::vector<fs::path> debris;
    for (const auto& entry : fs::recursive_directory_iterator(save)) {
        if (entry.path().filename().string().find("active-regions.arl.tmp.") == 0)
            debris.push_back(entry.path());
    }
    ASSERT_EQ(debris.size(), 1u);
    EXPECT_EQ(debris.front().parent_path(), save);
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
            for (auto* session : {&original, &control, &loaded, &explicit_load, &wrong_anchor})
                ASSERT_EQ(session->TickSimulation(dt), 1u);
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
    for (int i = 0; i < 317; ++i)
        ASSERT_EQ(control.TickSimulation(dt), 1u);
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
        for (auto* session : {&control, &loaded, &explicit_load})
            ASSERT_EQ(session->TickSimulation(dt), 1u);
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
    for (int i = 0; i < 5; ++i)
        ASSERT_EQ(legacy.TickSimulation(dt), 1u);
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
    for (int i = 0; i < 7; ++i)
        ASSERT_EQ(control.TickSimulation(dt), 1u);
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
            } else
                EXPECT_LT(session->GetEnergyFieldState()->at_cell(0, 0), 60000u);
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
    for (int i = 1; i < 8; ++i)
        ASSERT_EQ(original.TickSimulation(dt), 1u);
    ASSERT_EQ(original.GetEnergyFieldState()->page_count(), 0u);
    ASSERT_TRUE(original.SaveWorldState());
    EXPECT_FALSE(fs::exists(original.GetWorldSaveDir() / "aether_state.efs"));
    ASSERT_TRUE(loaded.LoadWorld(original.GetMetadata().worldId));
    EXPECT_EQ(loaded.GetEnergyFieldState()->total_raw(), 0u);
    EXPECT_EQ(loaded.GetEnergyFieldState()->next_fire_tick(), 16u);
    EXPECT_EQ(SessionWorldHash(loaded), SessionWorldHash(original));
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
        EXPECT_TRUE(session.SaveWorldStateTo(root.path() / "legacy-explicit"));
        EXPECT_FALSE(fs::exists(root.path() / "legacy-explicit"));
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
    chunk->heightmap_data = {8.0f + x, 8.5f + x, 9.0f + x};
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
    for (const int x : {0, 32})
        ASSERT_NO_FATAL_FAILURE(AdoptRegionGoldenChunk(session, x));
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
    for (int i = 0; i < 7; ++i)
        ASSERT_EQ(original.TickSimulation(original.GetSimulationClock().fixed_dt()), 1u);
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
    for (const auto& sample : combined)
        EXPECT_EQ(sample.work, 2u * 64u * 64u);
    for (const auto& stage : uninterrupted.GetSimBudgetTelemetry().SamplesByStage())
        EXPECT_TRUE(stage.empty());
}

static std::uint64_t Digest(const std::filesystem::path& save,
                            Luminumbra::world::RegionKey key,
                            std::vector<std::shared_ptr<Luminumbra::Chunk>> live,
                            std::vector<std::shared_ptr<Luminumbra::Chunk>> parked = {}) {
    return P::WorldSaveService::region_simulation_digest(
        save, key, P::RegionDigestInputs{std::move(live), std::move(parked)});
}

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
        chunk->heightmap_data = {8.0f + x};
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
    for (const auto key : owned_keys)
        EXPECT_TRUE(session.GetActiveRegionLedger().records().contains(key));
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
    for (const auto key : {resident, pristine, loading, pristine_loading})
        EXPECT_TRUE(session.GetActiveRegionLedger().records().contains(key));
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
    for (auto* session : {&walking, &noclip, &loaded})
        ASSERT_EQ(session->TickSimulation(session->GetSimulationClock().fixed_dt()), 1u);
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
    chunk->sdf_data.assign(17 * 17 * 17, 1.0f);
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
    for (std::uint64_t t = 1; t < start; ++t)
        ASSERT_EQ(session.TickSimulation(dt), 1u);
    ASSERT_FALSE(session.GetRegionSchedulerFault().has_value());
    // Half a tick of slack keeps the clock from rounding the frame down. A fault
    // on the first tick of the frame stops the second tick from running.
    ASSERT_EQ(session.TickSimulation(2.5 * dt), k - start + 1);
    const auto fault = session.GetRegionSchedulerFault();
    ASSERT_TRUE(fault.has_value());
    if (fault.has_value()) {
        const auto snapshot = fault.value();
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
    EXPECT_EQ(baseline, "49f694fd70645bcc");
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
