#include "../support/UniqueTempPath.h"
#include "../support/WorldgenTestData.h"
#include "luminumbra_client/app/RenderMeasurement.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/net/LockstepSession.h"
#include "luminumbra_common/persistence/SavedWorldCatalog.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/world/FarLodStore.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/world/TerrainPresetLoader.h"
#include "luminumbra_server/modes/ModeHelpers.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace {
namespace fs = std::filesystem;
using Luminumbra::world::GameSession;
using Luminumbra::world::LoadTerrainPresetFromJson;
using Luminumbra::world::ResolveEffectivePreset;
using Luminumbra::world::TerrainPresetUse;

class EffectivePresetIdentity : public ::testing::Test {
protected:
    fs::path root;
    Luminumbra::JobSystem jobs;
    nlohmann::json shipping;
    ServerCliOptions options;

    void SetUp() override {
        root = Luminumbra::TestSupport::CreateUniqueTempDirectory("effective_preset_");
        Luminumbra::TestSupport::CopyWorldgenTestData(LUMINUMBRA_SOURCE_ROOT, root);
        fs::create_directories(root / "worlds/atlas/presets");
        std::ifstream input(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets/default.json");
        ASSERT_TRUE(input);
        input >> shipping;
        WriteCatalog(shipping);
        options.root = root.string();
        options.systems_config_override = luminumbra::core::SystemConfig{};
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
        session.ApplySystemConfig(*options.systems_config_override);
        session.SetFiniteHydrologyEnabled(RunnerConfigFrom(options).finite_hydrology);
    }
    void WriteCatalog(const nlohmann::json& preset) {
        std::ofstream output(root / "worlds/atlas/presets/default.json");
        output << preset.dump() << '\n';
        ASSERT_TRUE(output.good());
    }
    void ExpectInvalidClientPresetsRefused() {
        const auto host = RuntimeHelloFrom(options);
        ASSERT_FALSE(host.preset_content_hash.empty());
        const auto complete = shipping.dump();
        const std::vector<std::string> invalid_presets{"not JSON",
                                                       complete.substr(0, complete.size() / 2),
                                                       R"({"schema_rev":1e1000})",
                                                       R"({"schema_rev":"eight"})",
                                                       "null"};
        for (const auto& invalid : invalid_presets) {
            SCOPED_TRACE(invalid);
            for (const bool override_preset : {false, true}) {
                SCOPED_TRACE(override_preset);
                // Both clients use RuntimeHelloFrom immediately before sending
                // their Hello, without loading or validating a world first.
                {
                    std::ofstream output(root / "worlds/atlas/presets/default.json");
                    output << (override_preset ? complete : invalid);
                    ASSERT_TRUE(output.good());
                }
                options.preset_json_override =
                    override_preset ? std::optional<std::string>(invalid) : std::nullopt;
                Luminumbra::Net::HelloMsg client;
                ASSERT_NO_THROW(client = RuntimeHelloFrom(options));
                EXPECT_TRUE(client.preset_content_hash.empty());
                EXPECT_EQ(client.system_config_identity, host.system_config_identity);
                Luminumbra::Net::HelloMsg decoded;
                ASSERT_TRUE(
                    Luminumbra::Net::DecodeHello(Luminumbra::Net::EncodeHello(client), decoded));
                EXPECT_FALSE(Luminumbra::Net::HelloMatches(host, decoded));
                EXPECT_FALSE(Luminumbra::Net::HelloMatches(decoded, host));
                EXPECT_FALSE(Luminumbra::Net::HelloMatches(decoded, decoded));
            }
        }
    }
};

TEST_F(EffectivePresetIdentity, TcpJoinRefusesMalformedAndTruncatedPresets) {
    options.net_join = true;
    ExpectInvalidClientPresetsRefused();
}

TEST_F(EffectivePresetIdentity, SoakClientRefusesMalformedAndTruncatedPresets) {
    options.net_soak_client = true;
    ExpectInvalidClientPresetsRefused();
}

TEST_F(EffectivePresetIdentity, FileLoaderRefusesMalformedTruncatedAndOverflowingJson) {
    const auto complete = shipping.dump();
    for (const auto& invalid : {std::string("not JSON"),
                                complete.substr(0, complete.size() / 2),
                                std::string(R"({"schema_rev":1e1000})")}) {
        SCOPED_TRACE(invalid);
        {
            std::ofstream output(root / "worlds/atlas/presets/default.json");
            output << invalid;
            ASSERT_TRUE(output.good());
        }
        Luminumbra::world::TerrainPresetLoadResult loaded;
        ASSERT_NO_THROW(loaded = Luminumbra::world::LoadTerrainPreset(
                            root / "worlds/atlas/presets/default.json"));
        EXPECT_FALSE(loaded.ok);
        EXPECT_TRUE(loaded.preset_content_hash.empty());
        ASSERT_EQ(loaded.errors.size(), 1u);
        EXPECT_NE(loaded.errors.front().find("failed to parse world preset JSON"),
                  std::string::npos);
    }
}

TEST_F(EffectivePresetIdentity, TcpHostAndDefaultJoinAgreeIncludingUnstampedOverrides) {
    for (const bool unstamped : {false, true}) {
        SCOPED_TRACE(unstamped);
        auto source = shipping;
        if (unstamped) {
            source["schema_rev"] = 7;
            source["generation_params"]["terrain"].erase("surface_sampling_revision");
        }
        WriteCatalog(source);
        for (const bool override_preset : {false, true}) {
            SCOPED_TRACE(override_preset);
            options.preset_json_override =
                override_preset ? std::optional<std::string>(source.dump()) : std::nullopt;
            GameSession host;
            Configure(host);
            ASSERT_TRUE(host.CreateWorld(
                "Host",
                options.seed,
                options.preset,
                options.preset_json_override ? &*options.preset_json_override : nullptr));
            auto advertised = RuntimeHelloFrom(options);
            // Same admission identity fields as NetTcp, NetRuntime and NetSoak.
            advertised.system_config_identity = host.SystemConfigIdentity();
            advertised.preset_content_hash = host.PresetContentHash();
            const auto join = RuntimeHelloFrom(options);
            Luminumbra::Net::HelloMsg decoded;
            ASSERT_TRUE(Luminumbra::Net::DecodeHello(Luminumbra::Net::EncodeHello(join), decoded));
            EXPECT_TRUE(Luminumbra::Net::HelloMatches(advertised, decoded));
            const auto saved =
                Luminumbra::Persistence::InspectSavedWorld(root, host.GetMetadata().worldId);
            ASSERT_TRUE(saved.error.empty()) << saved.error;
            const auto catalog =
                LoadTerrainPresetFromJson(nlohmann::json::parse(saved.preset_json), root / "data");
            ASSERT_TRUE(catalog.ok);
            EXPECT_EQ(catalog.preset_content_hash, join.preset_content_hash);
            EXPECT_EQ(catalog.params.surface_sampling_revision, 2);
            // The previous binary's enforced maximum is 7: it refuses this
            // embedded preset before accepting its identity or generating data.
            EXPECT_EQ(nlohmann::json::parse(saved.preset_json).at("schema_rev"), 8);
            EXPECT_GT(nlohmann::json::parse(saved.preset_json).at("schema_rev").get<int>(), 7);
        }
    }
}

TEST_F(EffectivePresetIdentity, ExplicitRevisionOneIsPreservedAndCannotJoinRevisionTwo) {
    auto historical = shipping;
    historical["description"] = "Historical \xc3\xa9 landscape";
    historical["schema_rev"] = 7;
    historical["generation_params"]["terrain"]["surface_sampling_revision"] = 1;
    options.preset_json_override = historical.dump();
    const auto old_hello = RuntimeHelloFrom(options);
    GameSession historical_world;
    Configure(historical_world);
    ASSERT_TRUE(historical_world.CreateWorld(
        "Historical", options.seed, options.preset, &*options.preset_json_override));
    EXPECT_EQ(historical_world.GetWorldSystem()->get_params().surface_sampling_revision, 1);
    EXPECT_EQ(historical_world.PresetContentHash(), old_hello.preset_content_hash);
    const auto saved =
        Luminumbra::Persistence::InspectSavedWorld(root, historical_world.GetMetadata().worldId);
    ASSERT_TRUE(saved.error.empty()) << saved.error;
    EXPECT_EQ(nlohmann::json::parse(saved.preset_json).at("schema_rev"), 7);
    EXPECT_EQ(nlohmann::json::parse(saved.preset_json).at("description"),
              historical.at("description"));
    EXPECT_EQ(historical_world.PresetContentHash(),
              LoadTerrainPresetFromJson(historical, root / "data").preset_content_hash);
    options.preset_json_override.reset();
    const auto new_hello = RuntimeHelloFrom(options);
    EXPECT_NE(old_hello.preset_content_hash, new_hello.preset_content_hash);
    EXPECT_FALSE(Luminumbra::Net::HelloMatches(new_hello, old_hello));
    EXPECT_FALSE(Luminumbra::Net::HelloMatches(old_hello, new_hello));
}

TEST_F(EffectivePresetIdentity, TransientWorldsHashTheResolvedSelector) {
    auto source = shipping;
    source["schema_rev"] = 7;
    source["generation_params"]["terrain"].erase("surface_sampling_revision");
    std::string current_hash;
    for (const bool explicit_one : {false, true}) {
        SCOPED_TRACE(explicit_one);
        if (explicit_one) {
            source["schema_rev"] = 8;
            source["generation_params"]["terrain"]["surface_sampling_revision"] = 1;
        }
        WriteCatalog(source);
        GameSession transient;
        Configure(transient);
        ASSERT_TRUE(transient.CreateTransientWorld("Transient", options.seed, options.preset));
        const auto expected = LoadTerrainPresetFromJson(
            ResolveEffectivePreset(source, TerrainPresetUse::NewWorld), root / "data");
        ASSERT_TRUE(expected.ok);
        EXPECT_EQ(transient.GetWorldSystem()->get_params().surface_sampling_revision,
                  explicit_one ? 1 : 2);
        EXPECT_EQ(transient.PresetContentHash(), expected.preset_content_hash);
        EXPECT_EQ(transient.PresetContentHash(), RuntimeHelloFrom(options).preset_content_hash);
        if (explicit_one)
            EXPECT_NE(transient.PresetContentHash(), current_hash);
        else
            current_hash = transient.PresetContentHash();
    }
}

TEST_F(EffectivePresetIdentity, SurfaceFlightCaptureMatchesSavedAndGeneratedIdentity) {
    std::ifstream input(fs::path(LUMINUMBRA_SOURCE_ROOT) /
                        "tools/perf/fixtures/surface-flight.traversal");
    const auto traversal = Luminumbra::Client::Measurement::Traversal::read(input);
    GameSession session;
    Configure(session);
    ASSERT_TRUE(session.CreateWorld("Capture", std::to_string(traversal.seed), traversal.preset));
    const auto saved =
        Luminumbra::Persistence::InspectSavedWorld(root, session.GetMetadata().worldId);
    ASSERT_TRUE(saved.error.empty()) << saved.error;
    const auto preset = nlohmann::json::parse(saved.preset_json);
    const auto* world = session.GetWorldSystem();
    EXPECT_EQ(static_cast<std::uint32_t>(world->get_seed()), traversal.seed);
    EXPECT_EQ(session.GetMetadata().worldType, traversal.preset);
    EXPECT_EQ(preset.at("schema_rev"), traversal.presetRevision);
    EXPECT_EQ(Luminumbra::Persistence::StableChecksum(preset.dump()), traversal.presetIdentity);
    EXPECT_EQ(std::to_string(Luminumbra::World::ComputeTerrainParamsHash(world->get_params(),
                                                                         world->get_seed())),
              traversal.contentIdentity);
}
} // namespace
