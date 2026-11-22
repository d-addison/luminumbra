#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/game/WorldClockAuthorityAdapter.h"
#include "luminumbra_common/persistence/AuthorityContributor.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/replay/ReplayStream.h"

#include "../support/CalendarMetadataCases.h"
#include "../support/UniqueTempPath.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace P = Luminumbra::Persistence;

class Contributor final : public P::IAuthorityStateContributor {
public:
    Contributor(std::string id, std::string state)
        : m_id(std::move(id))
        , bytes(std::move(state)) {}

    std::string_view contributor_id() const noexcept override {
        return m_id;
    }
    std::string_view canonical_state() const noexcept override {
        ++reads;
        return bytes;
    }

private:
    std::string m_id;

public:
    std::string bytes;
    mutable std::size_t reads = 0;
};

std::string WorldHash(const P::AuthorityContributorRegistry& registry) {
    return P::ComposeWorldHash(
        "chunk", "wind", "weather", "aether", "scent", "ecology", "plant", registry);
}

constexpr char kCanonicalPair[] = "|authorityContributors:v1:"
                                  "\x02\0\0\0\0\0\0\0"
                                  "\x05\0\0\0\0\0\0\0alpha"
                                  "\x01\0\0\0\0\0\0\0A"
                                  "\x04\0\0\0\0\0\0\0zeta"
                                  "\x01\0\0\0\0\0\0\0Z";

TEST(AuthorityContributor, EmptyRegistryPreservesLegacyGoldenHashes) {
    const P::AuthorityContributorRegistry registry;
    EXPECT_TRUE(registry.SerializeCanonicalState().empty());
    EXPECT_EQ(
        WorldHash(registry),
        P::ComposeWorldHash("chunk", "wind", "weather", "aether", "scent", "ecology", "plant"));
    EXPECT_EQ(WorldHash(registry), "76950c71eb8bf95e");
    EXPECT_EQ(P::ComposeWorldHash("", "", "", "", "", "", "", registry), "9deb9f80f9012248");
}

TEST(AuthorityContributor, OnlyEmptyStatesPreserveLegacyGoldenHashes) {
    Contributor alpha("alpha", "");
    Contributor zeta("zeta", "");
    P::AuthorityContributorRegistry registry;
    ASSERT_TRUE(registry.Register(zeta));
    ASSERT_TRUE(registry.Register(alpha));
    EXPECT_TRUE(registry.SerializeCanonicalState().empty());
    EXPECT_TRUE(alpha.ComputeHash().empty());
    EXPECT_EQ(WorldHash(registry), "76950c71eb8bf95e");
    EXPECT_EQ(P::ComposeWorldHash("", "", "", "", "", "", "", registry), "9deb9f80f9012248");
}

TEST(AuthorityContributor, CanonicalBytesAndHashIgnoreEveryRegistrationPermutation) {
    Contributor alpha("alpha", "A");
    Contributor empty("middle", "");
    Contributor zeta("zeta", "Z");
    std::array<const Contributor*, 3> contributors{&alpha, &empty, &zeta};
    std::array<std::size_t, 3> order{0, 1, 2};
    do {
        P::AuthorityContributorRegistry registry;
        for (const auto index : order) {
            ASSERT_TRUE(registry.Register(*contributors[index]));
        }
        EXPECT_EQ(registry.SerializeCanonicalState(),
                  std::string(kCanonicalPair, sizeof(kCanonicalPair) - 1));
        EXPECT_EQ(WorldHash(registry), "50ef200c7aa942c8");
    } while (std::next_permutation(order.begin(), order.end()));
    EXPECT_EQ(alpha.ComputeHash(), "af63fc4c860222ec");
}

TEST(AuthorityContributor, StateAndIdBoundariesAndEmbeddedZeroBytesAffectHash) {
    Contributor first("a", "bc");
    Contributor second("ab", "c");
    P::AuthorityContributorRegistry left;
    P::AuthorityContributorRegistry right;
    ASSERT_TRUE(left.Register(first));
    ASSERT_TRUE(right.Register(second));
    EXPECT_NE(left.SerializeCanonicalState(), right.SerializeCanonicalState());
    EXPECT_NE(WorldHash(left), WorldHash(right));
    second.bytes = first.bytes;
    EXPECT_NE(WorldHash(left), WorldHash(right));
    first.bytes = std::string("x\0y", 3);
    const auto before = WorldHash(left);
    const auto state_before = first.ComputeHash();
    first.bytes.back() = 'z';
    EXPECT_NE(WorldHash(left), before);
    EXPECT_NE(first.ComputeHash(), state_before);
    EXPECT_NE(WorldHash(left), "76950c71eb8bf95e");
    first.bytes.clear();
    EXPECT_EQ(WorldHash(left), "76950c71eb8bf95e");
}

TEST(AuthorityContributor, InvalidAndDuplicateIdsLeaveRegistryUnchanged) {
    Contributor original("valid:state-1.0_x", "state");
    P::AuthorityContributorRegistry registry;
    ASSERT_TRUE(registry.Register(original));
    const auto before = registry.SerializeCanonicalState();
    const std::vector<std::string> invalid{
        "",
        "Uppercase",
        "with space",
        "slash/path",
        "pipe|",
        std::string("a\0b", 3),
        std::string(1, '\xff'),
        std::string(P::AuthorityContributorRegistry::kMaxIdBytes + 1, 'a')};
    for (const auto& id : invalid) {
        Contributor rejected(id, "different");
        EXPECT_FALSE(registry.Register(rejected));
        EXPECT_EQ(registry.SerializeCanonicalState(), before);
    }
    Contributor duplicate("valid:state-1.0_x", "different");
    EXPECT_FALSE(registry.Register(duplicate));
    EXPECT_FALSE(registry.Register(original));
    EXPECT_EQ(registry.SerializeCanonicalState(), before);
    Contributor longest(std::string(P::AuthorityContributorRegistry::kMaxIdBytes, 'z'), "state");
    EXPECT_TRUE(registry.Register(longest));
}

TEST(AuthorityContributor, RegistrationBoundAndSingleReadPerContributor) {
    std::vector<Contributor> contributors;
    contributors.reserve(P::AuthorityContributorRegistry::kMaxContributors + 1);
    for (std::size_t index = 0; index <= P::AuthorityContributorRegistry::kMaxContributors;
         ++index) {
        contributors.emplace_back("id:" + std::to_string(index), "x");
    }
    P::AuthorityContributorRegistry registry;
    for (std::size_t index = 0; index < P::AuthorityContributorRegistry::kMaxContributors;
         ++index) {
        ASSERT_TRUE(registry.Register(contributors[index]));
    }
    EXPECT_FALSE(registry.Register(contributors.back()));
    const auto before = registry.SerializeCanonicalState();
    const std::string prefix = "|authorityContributors:v1:";
    EXPECT_EQ(before.substr(prefix.size(), 8), std::string("\0\x01\0\0\0\0\0\0", 8));
    for (std::size_t index = 0; index < P::AuthorityContributorRegistry::kMaxContributors;
         ++index) {
        EXPECT_EQ(contributors[index].reads, 1u);
    }
    EXPECT_EQ(contributors.back().reads, 0u);
    EXPECT_FALSE(registry.Register(contributors.front()));
    EXPECT_EQ(registry.SerializeCanonicalState(), before);
}

TEST(AuthorityContributor, CanonicalByteLimitIsExactAndOversizeCanRecover) {
    Contributor contributor("a", "");
    P::AuthorityContributorRegistry registry;
    ASSERT_TRUE(registry.Register(contributor));
    // Prefix, count, two lengths and the one-byte ID occupy exactly 51 bytes.
    contributor.bytes.assign(P::AuthorityContributorRegistry::kMaxCanonicalBytes - 51, 'x');
    EXPECT_EQ(registry.SerializeCanonicalState().size(),
              P::AuthorityContributorRegistry::kMaxCanonicalBytes);
    contributor.bytes.push_back('x');
    EXPECT_THROW(registry.SerializeCanonicalState(), std::length_error);
    EXPECT_THROW(WorldHash(registry), std::length_error);
    contributor.bytes.assign(P::AuthorityContributorRegistry::kMaxCanonicalBytes + 1, 'x');
    EXPECT_THROW(contributor.ComputeHash(), std::length_error);
    EXPECT_THROW(registry.SerializeCanonicalState(), std::length_error);
    contributor.bytes.clear();
    EXPECT_EQ(WorldHash(registry), "76950c71eb8bf95e");
}

TEST(AuthorityContributor, AggregateSizeLimitIncludesEveryRecord) {
    Contributor first("a", "x");
    Contributor second("b", "x");
    P::AuthorityContributorRegistry registry;
    ASSERT_TRUE(registry.Register(first));
    ASSERT_TRUE(registry.Register(second));
    first.bytes.assign(P::AuthorityContributorRegistry::kMaxCanonicalBytes / 2, 'x');
    second.bytes.assign(P::AuthorityContributorRegistry::kMaxCanonicalBytes / 2, 'y');
    EXPECT_THROW(registry.SerializeCanonicalState(), std::length_error);
    second.bytes.clear();
    EXPECT_FALSE(registry.SerializeCanonicalState().empty());
    first.bytes.assign(P::AuthorityContributorRegistry::kMaxCanonicalBytes - 51, 'x');
    EXPECT_EQ(registry.SerializeCanonicalState().size(),
              P::AuthorityContributorRegistry::kMaxCanonicalBytes);
    second.bytes = "y";
    EXPECT_THROW(registry.SerializeCanonicalState(), std::length_error);
}

using ClockAdapter = Luminumbra::Game::WorldClockAuthorityAdapter;
using Luminumbra::ModuleRegistry;
using Luminumbra::ModuleResult;
using Luminumbra::world::WorldCalendar;
using Luminumbra::world::WorldClock;

void RegisterModule(ModuleRegistry& modules, std::string_view id) {
    ASSERT_EQ(modules.register_module({.id = std::string(id),
                                       .provides = {},
                                       .requires_services = {},
                                       .startup = nullptr,
                                       .shutdown = nullptr,
                                       .context = nullptr}),
              ModuleResult::Ok);
}

TEST(AuthorityContributor, ClockAdapterPreservesLegacyAndCurrentBytesAndRoundTrips) {
    for (const auto calendar :
         {WorldCalendar::Legacy(), WorldCalendar{}, WorldCalendar{123456, 17, 3, 7, 19}}) {
        for (const auto tick :
             {std::uint64_t{0}, std::uint64_t{12345}, WorldClock::kTickLimit - 1}) {
            const WorldClock original(tick, calendar);
            nlohmann::json expected{{"unrelated", "retained"}};
            original.write_metadata(expected);
            nlohmann::json actual{{"unrelated", "retained"}};
            ClockAdapter::WriteMetadata(original, actual);
            EXPECT_EQ(actual.dump(4), expected.dump(4));
            EXPECT_EQ(ClockAdapter::CanonicalBytes(original), original.canonical_bytes());
            WorldClock restored;
            std::string error;
            ASSERT_TRUE(
                ClockAdapter::ReadMetadata(nlohmann::json::parse(actual.dump()), restored, error));
            EXPECT_TRUE(error.empty());
            EXPECT_EQ(restored.canonical_bytes(), original.canonical_bytes());
        }
    }
    const WorldClock legacy(12345, WorldCalendar::Legacy());
    const std::string pinned = std::string("world_clock:v1:") +
                               std::string("\x39\x30\0\0\0\0\0\0\xc0\x4b\x03\0\x08\0\0\0", 16);
    EXPECT_EQ(ClockAdapter::CanonicalBytes(legacy), pinned);
    nlohmann::json metadata;
    ClockAdapter::WriteMetadata(legacy, metadata);
    EXPECT_EQ(
        metadata.dump(),
        "{\"calendar\":{\"dayLengthTicks\":216000,\"daysPerYear\":8},\"simulationTick\":12345}");
}

TEST(AuthorityContributor, ClockAdapterPreservesComposedCheckpointAndReplayBytes) {
    const WorldClock clock(12345, WorldCalendar::Legacy());
    const auto compose = [](const std::string& bytes) {
        return P::ComposeWorldHash("chunk",
                                   "wind",
                                   "weather",
                                   "aether",
                                   "scent",
                                   P::StableChecksum("ecology" + bytes + "ledger-ground-fields"),
                                   "plant");
    };
    const auto legacy_hash = compose(clock.canonical_bytes());
    const auto registered_hash = compose(ClockAdapter::CanonicalBytes(clock));
    EXPECT_EQ(registered_hash, legacy_hash);
    const Luminumbra::TestSupport::ScopedTempFile legacy_file("clock_legacy_", ".lrec1");
    const Luminumbra::TestSupport::ScopedTempFile registered_file("clock_registered_", ".lrec1");
    const std::array paths{legacy_file.Path(), registered_file.Path()};
    const std::array hashes{legacy_hash, registered_hash};
    for (std::size_t index = 0; index < paths.size(); ++index) {
        Luminumbra::Replay::ReplayWriter writer;
        Luminumbra::Replay::ReplayHeader header{};
        header.seed = 42;
        header.start_world_hash = hashes[index];
        ASSERT_TRUE(
            writer.Open(Luminumbra::Filesystem::GenericUtf8PathString(paths[index]), header));
        writer.RecordInput(1, {1, 2, 3});
        writer.RecordCheckpoint({.tick = 1,
                                 .world_hash = hashes[index],
                                 .terrain = "terrain",
                                 .water = "water",
                                 .entities = "entities"});
        ASSERT_TRUE(writer.Finalize(1));
    }
    std::ifstream legacy_input(paths[0], std::ios::binary);
    std::ifstream registered_input(paths[1], std::ios::binary);
    ASSERT_TRUE(legacy_input.is_open());
    ASSERT_TRUE(registered_input.is_open());
    const std::string legacy_bytes{std::istreambuf_iterator<char>(legacy_input), {}};
    const std::string registered_bytes{std::istreambuf_iterator<char>(registered_input), {}};
    ASSERT_FALSE(legacy_input.bad());
    ASSERT_FALSE(registered_input.bad());
    ASSERT_FALSE(legacy_bytes.empty());
    EXPECT_EQ(registered_bytes, legacy_bytes);
}

class MetadataContributor final : public P::IAuthorityMetadataContributor {
public:
    std::string_view contributor_id() const noexcept override {
        return "test:metadata";
    }
    std::string_view canonical_state() const noexcept override {
        return bytes;
    }
    void WriteMetadata(nlohmann::json& metadata) const override {
        ++writes;
        metadata["test"] = true;
    }
    bool PrepareRestore(const nlohmann::json&, std::string& error) override {
        ++preparations;
        error = "refused metadata";
        return false;
    }
    void CommitRestore() noexcept override {
        ++commits;
    }
    mutable unsigned writes = 0;
    unsigned preparations = 0;
    unsigned commits = 0;
    std::string bytes = "state";
};

TEST(AuthorityContributor, RequiredIdentitiesRefuseBeforeEveryCallbackOrWrite) {
    const std::array<std::string_view, 1> required{"test.module.v1"};
    for (const auto id : {"", "unknown.module", "test.module.v2", "test.module.v1"}) {
        ModuleRegistry modules;
        if (std::string_view(id) != "") {
            RegisterModule(modules, id);
        }
        ASSERT_EQ(modules.startup(), ModuleResult::Ok);
        MetadataContributor contributor;
        P::AuthorityContributorRegistry registry;
        ASSERT_TRUE(registry.RegisterMetadata(contributor, "test.module.v1"));
        const std::array<std::string_view, 1> unknown{"unknown.required"};
        // The matching identity is deliberately stopped; all other cases lack it.
        if (std::string_view(id) == "test.module.v1") {
            ASSERT_EQ(modules.shutdown(), ModuleResult::Ok);
        }
        for (const auto requirements : {std::span<const std::string_view>(required),
                                        std::span<const std::string_view>(unknown),
                                        std::span<const std::string_view>{}}) {
            nlohmann::json metadata{{"preserved", true}};
            const auto before = metadata.dump();
            std::string error;
            EXPECT_FALSE(registry.WriteMetadata(modules, requirements, metadata));
            EXPECT_FALSE(registry.RestoreMetadata(modules, requirements, metadata, error));
            EXPECT_FALSE(error.empty());
            EXPECT_THROW(
                (void)registry.LegacyCanonicalState(modules, requirements, "test:metadata"),
                std::invalid_argument);
            EXPECT_EQ(metadata.dump(), before);
        }
        EXPECT_EQ(contributor.writes, 0u);
        EXPECT_EQ(contributor.preparations, 0u);
        EXPECT_EQ(contributor.commits, 0u);
    }
}

TEST(AuthorityContributor, MissingBindingUnknownRequirementAndLimitsRefuse) {
    ModuleRegistry modules;
    RegisterModule(modules, ClockAdapter::kModuleId);
    RegisterModule(modules, "other.module");
    ASSERT_EQ(modules.startup(), ModuleResult::Ok);
    EXPECT_TRUE(modules.contains(ClockAdapter::kModuleId));
    EXPECT_FALSE(modules.contains("luminumbra.world_clock.v2"));
    P::AuthorityContributorRegistry registry;
    EXPECT_FALSE(registry.ValidateRequiredModules(modules, ClockAdapter::kRequiredModules));
    WorldClock clock(7);
    ClockAdapter adapter(clock);
    EXPECT_FALSE(registry.RegisterMetadata(adapter, ""));
    EXPECT_FALSE(registry.RegisterMetadata(adapter, "invalid/id"));
    ASSERT_TRUE(registry.RegisterMetadata(adapter, ClockAdapter::kModuleId));
    EXPECT_FALSE(registry.RegisterMetadata(adapter, ClockAdapter::kModuleId));
    EXPECT_TRUE(registry.ValidateRequiredModules(modules, ClockAdapter::kRequiredModules));
    const std::array<std::string_view, 1> unbound{"other.module"};
    const std::array<std::string_view, 1> unknown{"unknown.module"};
    const std::array<std::string_view, 1> invalid{""};
    EXPECT_FALSE(registry.ValidateRequiredModules(modules, unbound));
    EXPECT_FALSE(registry.ValidateRequiredModules(modules, unknown));
    EXPECT_FALSE(registry.ValidateRequiredModules(modules, invalid));
    EXPECT_EQ(registry.LegacyCanonicalState(
                  modules, ClockAdapter::kRequiredModules, ClockAdapter::kContributorId),
              clock.canonical_bytes());
    std::vector<std::string_view> requirements(P::AuthorityContributorRegistry::kMaxContributors,
                                               ClockAdapter::kModuleId);
    EXPECT_TRUE(registry.ValidateRequiredModules(modules, requirements));
    requirements.push_back(ClockAdapter::kModuleId);
    EXPECT_FALSE(registry.ValidateRequiredModules(modules, requirements));
    EXPECT_THROW((void)registry.LegacyCanonicalState(modules, {}, "missing:contributor"),
                 std::invalid_argument);
    Contributor canonical_only("canonical:only", "x");
    ASSERT_TRUE(registry.Register(canonical_only));
    EXPECT_THROW((void)registry.LegacyCanonicalState(modules, {}, "canonical:only"),
                 std::invalid_argument);
}

TEST(AuthorityContributor, UnknownRequirementsPrecedeCallbacksWithLiveRegisteredModules) {
    ModuleRegistry modules;
    RegisterModule(modules, "test.module");
    ASSERT_EQ(modules.startup(), ModuleResult::Ok);
    MetadataContributor contributor;
    P::AuthorityContributorRegistry registry;
    ASSERT_TRUE(registry.RegisterMetadata(contributor, "test.module"));
    const std::array<std::string_view, 1> unknown{"unknown.module"};
    nlohmann::json metadata{{"preserved", true}};
    const auto before = metadata.dump();
    std::string error;
    EXPECT_FALSE(registry.WriteMetadata(modules, unknown, metadata));
    EXPECT_FALSE(registry.RestoreMetadata(modules, unknown, metadata, error));
    EXPECT_EQ(metadata.dump(), before);
    EXPECT_EQ(contributor.writes, 0u);
    EXPECT_EQ(contributor.preparations, 0u);
    EXPECT_EQ(contributor.commits, 0u);
    ASSERT_TRUE(registry.WriteMetadata(modules, {}, metadata));
    EXPECT_EQ(contributor.writes, 1u);
    EXPECT_TRUE(metadata.at("test").get<bool>());
    contributor.bytes.assign(P::AuthorityContributorRegistry::kMaxCanonicalBytes + 1, 'x');
    EXPECT_THROW((void)registry.LegacyCanonicalState(modules, {}, "test:metadata"),
                 std::length_error);
}

TEST(AuthorityContributor, ClockRestoreStagesAllContributorsBeforeCommitAndCanRetry) {
    ModuleRegistry modules;
    RegisterModule(modules, ClockAdapter::kModuleId);
    RegisterModule(modules, "test.module");
    ASSERT_EQ(modules.startup(), ModuleResult::Ok);
    WorldClock clock(7);
    ClockAdapter adapter(clock);
    MetadataContributor refusing;
    P::AuthorityContributorRegistry registry;
    ASSERT_TRUE(registry.RegisterMetadata(adapter, ClockAdapter::kModuleId));
    ASSERT_TRUE(registry.RegisterMetadata(refusing, "test.module"));
    nlohmann::json metadata;
    WorldClock(42).write_metadata(metadata);
    std::string error;
    EXPECT_FALSE(
        registry.RestoreMetadata(modules, ClockAdapter::kRequiredModules, metadata, error));
    EXPECT_EQ(clock.tick(), 7u);
    EXPECT_EQ(adapter.canonical_state(), clock.canonical_bytes());
    EXPECT_EQ(refusing.preparations, 1u);
    EXPECT_EQ(refusing.commits, 0u);
    P::AuthorityContributorRegistry admitted;
    ASSERT_TRUE(admitted.RegisterMetadata(adapter, ClockAdapter::kModuleId));
    ASSERT_TRUE(admitted.RestoreMetadata(modules, ClockAdapter::kRequiredModules, metadata, error));
    EXPECT_EQ(clock.tick(), 42u);
    EXPECT_EQ(adapter.canonical_state(), clock.canonical_bytes());
    nlohmann::json saved;
    ASSERT_TRUE(admitted.WriteMetadata(modules, ClockAdapter::kRequiredModules, saved));
    EXPECT_EQ(saved, metadata);
}

TEST(AuthorityContributor, ClockAdapterRetainsMissingDefaultsAndRejectsMalformedInputUnchanged) {
    WorldClock clock(7);
    std::string error;
    ASSERT_TRUE(ClockAdapter::ReadMetadata(nlohmann::json::object(), clock, error));
    EXPECT_EQ(clock.canonical_bytes(), WorldClock(0, WorldCalendar::Legacy()).canonical_bytes());
    const auto before = clock.canonical_bytes();
    auto invalid = Luminumbra::TestSupport::InvalidCalendarMetadata();
    invalid.push_back(nullptr);
    invalid.push_back(nlohmann::json::array());
    invalid.push_back({{"simulationTick", -1}});
    invalid.push_back({{"simulationTick", "0"}});
    invalid.push_back({{"simulationTick", WorldClock::kTickLimit}});
    for (const auto& metadata : invalid) {
        EXPECT_FALSE(ClockAdapter::ReadMetadata(metadata, clock, error));
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(clock.canonical_bytes(), before);
    }
}
} // namespace
