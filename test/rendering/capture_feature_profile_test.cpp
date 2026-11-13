#include "../support/UniqueTempPath.h"
#include "core/RuntimeScenarioConfig.h"
#include "rendering/CaptureFeatureProfile.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <vector>

namespace {
using namespace Luminumbra::Rendering;
using namespace Luminumbra::Client::ScenarioHarness;
using Selection = RuntimeScenarioConfig::FeatureProfile;
using Change = CaptureFeatureChange;
using Capability = CaptureFeatureCapability;
using Feature = CaptureFeature;

RuntimeScenarioConfig AllFeatures() {
    RuntimeScenarioConfig runtime;
    runtime.capture_feature_profile = Selection::AllFeatures;
    return runtime;
}

CaptureFeatureContext EnabledContext() {
    CaptureFeatureContext context;
    context.asset_hash = {"resolved-content-hash", ""};
    context.config_hash = {"resolved-config-hash", ""};
    for (auto& adapter : context.adapters) {
        adapter = {.enabled = true, .change = Change::Unchanged, .reason = "final_setting_enabled"};
    }
    return context;
}

CaptureFeatureAdapter& Adapter(CaptureFeatureContext& context, Feature feature) {
    return context.adapters.at(static_cast<std::size_t>(feature));
}

const CaptureFeatureState& State(const CaptureFeatureProfile& profile, Feature feature) {
    return profile.features().at(static_cast<std::size_t>(feature));
}

CaptureFeatureProfile Resolve(const RuntimeScenarioConfig& runtime = AllFeatures(),
                              const CaptureFeatureContext& context = CaptureFeatureContext{},
                              const luminumbra::core::SystemConfig& systems = {},
                              const luminumbra::core::GraphicsValues& graphics = {}) {
    return CaptureFeatureProfile::Resolve(runtime, systems, graphics, context);
}

std::vector<char*> Arguments(std::vector<std::string>& arguments) {
    std::vector<char*> argv;
    for (auto& argument : arguments) {
        argv.push_back(argument.data());
    }
    return argv;
}

TEST(CaptureFeatureProfile, SelectsStrictProfileThroughBothRuntimeParsers) {
    const auto root = std::filesystem::current_path();
    for (const auto& arguments : std::vector<std::vector<std::string>>{
             {"client"},
             {"client", "--capture-feature-profile=none"},
             {"client", "--capture-feature-profile", "all_features"},
             {"client", "--capture-feature-profile=all_features"}}) {
        auto storage = arguments;
        auto argv = Arguments(storage);
        const auto argc = static_cast<int>(argv.size());
        const auto expected =
            arguments.size() > 1 && arguments.back() != "--capture-feature-profile=none"
                ? Selection::AllFeatures
                : Selection::None;
        EXPECT_EQ(ParseCaptureFeatureProfile(argc, argv.data()), expected);
        EXPECT_EQ(ParseRuntimeScenarioConfig(argc, argv.data(), root).capture_feature_profile,
                  expected);
        EXPECT_NO_THROW((void)ParseRuntimePathOptions(argc, argv.data()));
    }
}

TEST(CaptureFeatureProfile, RefusesMalformedSelectionsBeforeSaveRootCreation) {
    const auto directory = Luminumbra::TestSupport::CreateUniqueTempDirectory("feature-profile-");
    const auto output = directory / "uncreated";
    for (const auto& suffix : std::vector<std::vector<std::string>>{
             {"--capture-feature-profile"},
             {"--capture-feature-profile="},
             {"--capture-feature-profile", "--no-ui"},
             {"--capture-feature-profile=0"},
             {"--capture-feature-profile=ALL_FEATURES"},
             {"--capture-feature-profile=all_features_extra"},
             {"--capture-feature-profile=all_features", "--capture-feature-profile=none"},
             {"--capture-feature-profile=none", "--capture-feature-profile", "none"}}) {
        std::vector<std::string> storage{"client", "--save-root", output.string()};
        storage.insert(storage.end(), suffix.begin(), suffix.end());
        auto argv = Arguments(storage);
        const auto argc = static_cast<int>(argv.size());
        EXPECT_THROW((void)ParseCaptureFeatureProfile(argc, argv.data()), std::invalid_argument);
        EXPECT_THROW((void)ParseRuntimePathOptions(argc, argv.data()), std::invalid_argument);
        EXPECT_THROW((void)ParseRuntimeScenarioConfig(argc, argv.data(), directory),
                     std::invalid_argument);
        EXPECT_FALSE(std::filesystem::exists(output));
    }
    std::filesystem::remove_all(directory);
}

TEST(CaptureFeatureProfile, AllFeaturesCatalogAndEveryModeDefaultToUnsupported) {
    constexpr std::array expected{"ambient_animals",
                                  "creature_spawning",
                                  "creature_ai",
                                  "creature_animation",
                                  "foliage",
                                  "plant_growth",
                                  "plant_wind",
                                  "procedural_plants",
                                  "water",
                                  "rivers",
                                  "waterfalls",
                                  "hydrology",
                                  "weather_hydrology",
                                  "caves",
                                  "cave_flourishes",
                                  "structures",
                                  "weather",
                                  "weather_events",
                                  "live_weather_bridge",
                                  "precipitation",
                                  "particles",
                                  "ambient_particles",
                                  "precipitation_particles",
                                  "waterfall_spray",
                                  "lightning",
                                  "clouds",
                                  "cloud_shadows",
                                  "sky",
                                  "moon",
                                  "moonlight",
                                  "stars"};
    ASSERT_EQ(expected.size(), kCaptureFeatureCount);
    for (const auto mode : {CaptureFeatureMode::Timelapse,
                            CaptureFeatureMode::FrameScan,
                            CaptureFeatureMode::WorldVisualSweep,
                            CaptureFeatureMode::Scenario,
                            CaptureFeatureMode::TimeOfDaySweep,
                            CaptureFeatureMode::WeatherVisual,
                            CaptureFeatureMode::HeadlessScan,
                            CaptureFeatureMode::HeadlessScene}) {
        CaptureFeatureContext context;
        context.mode = mode;
        const auto profile = Resolve(AllFeatures(), context);
        EXPECT_EQ(profile.capability(), Capability::Unsupported);
        const auto manifest = nlohmann::json::parse(profile.Manifest());
        EXPECT_EQ(manifest.at("presence"), "not_evaluated");
        EXPECT_EQ(manifest.at("features").size(), expected.size());
        EXPECT_TRUE(manifest.at("asset_hash").at("value").is_null());
        EXPECT_FALSE(manifest.at("asset_hash").at("unavailable").get<std::string>().empty());
        for (std::size_t index = 0; index < expected.size(); ++index) {
            const auto& state = profile.features()[index];
            EXPECT_EQ(CaptureFeatureName(static_cast<Feature>(index)), expected[index]);
            EXPECT_TRUE(state.requested);
            EXPECT_FALSE(state.effective.has_value());
            EXPECT_EQ(state.change, Change::Unavailable);
            EXPECT_EQ(state.capability, Capability::Unsupported);
            EXPECT_EQ(state.reason, "adapter_unimplemented");
            EXPECT_EQ(manifest.at("features").at(index).at("feature"), expected[index]);
            EXPECT_TRUE(manifest.at("features").at(index).at("effective").is_null());
        }
    }
}

TEST(CaptureFeatureProfile, SupportedSettingsSerializeDeterministicallyWithoutPresenceClaims) {
    const auto context = EnabledContext();
    const auto profile = Resolve(AllFeatures(), context);
    EXPECT_EQ(profile.capability(), Capability::Supported);
    EXPECT_EQ(profile.Manifest(), Resolve(AllFeatures(), context).Manifest());
    std::ostringstream stream;
    EXPECT_TRUE(profile.WriteManifest(stream));
    EXPECT_EQ(stream.str(), profile.Manifest());
    const auto manifest = nlohmann::json::parse(stream.str());
    EXPECT_EQ(manifest.at("schema"), "luminumbra.capture_capabilities.v1");
    EXPECT_EQ(manifest.at("profile"), "all_features");
    EXPECT_EQ(manifest.at("capability"), "supported");
    EXPECT_EQ(manifest.at("config_hash").at("value"), context.config_hash.value);
    EXPECT_EQ(manifest.at("asset_hash").at("value"), context.asset_hash.value);
    EXPECT_EQ(manifest.at("presence"), "not_evaluated");
    EXPECT_FALSE(manifest.contains("complete"));
    EXPECT_FALSE(manifest.contains("counts"));
    for (const auto& entry : manifest.at("features")) {
        EXPECT_EQ(entry.at("requested"), true);
        EXPECT_EQ(entry.at("effective"), true);
        EXPECT_EQ(entry.at("change"), "unchanged");
        EXPECT_EQ(entry.at("capability"), "supported");
    }
}

TEST(CaptureFeatureProfile, EveryMissingAdapterAndSuppressedFeaturePreventsSupport) {
    for (std::size_t index = 0; index < kCaptureFeatureCount; ++index) {
        auto context = EnabledContext();
        context.adapters[index] = {};
        auto profile = Resolve(AllFeatures(), context);
        EXPECT_EQ(profile.capability(), Capability::Unsupported);
        EXPECT_EQ(profile.features()[index].capability, Capability::Unsupported);
        context.adapters[index] = {
            .enabled = false, .change = Change::Suppressed, .reason = "final_pin_disabled"};
        profile = Resolve(AllFeatures(), context);
        EXPECT_EQ(profile.capability(), Capability::Refused);
        EXPECT_EQ(profile.features()[index].reason, "final_pin_disabled");
    }
}

TEST(CaptureFeatureProfile, ExplicitSubsetRecordsUnrequestedForcedAndSuppressedStates) {
    CaptureFeatureContext context;
    context.asset_hash = {"assets", ""};
    context.config_hash = {"config", ""};
    Adapter(context, Feature::Water) = {
        .enabled = true, .change = Change::Unchanged, .reason = "water_enabled"};
    Adapter(context,
            Feature::Stars) = {.enabled = true, .change = Change::Forced, .reason = "night_pin"};
    Adapter(context, Feature::Particles) = {
        .enabled = false, .change = Change::Suppressed, .reason = "particles_disabled"};
    Adapter(context, Feature::Moon) = {
        .enabled = false, .change = Change::Unchanged, .reason = "not_requested"};
    const std::array<std::string_view, 1> requested{"water"};
    const auto profile = CaptureFeatureProfile::Resolve({}, {}, {}, context, requested);
    EXPECT_EQ(profile.capability(), Capability::Supported);
    EXPECT_TRUE(State(profile, Feature::Water).requested);
    EXPECT_FALSE(State(profile, Feature::Stars).requested);
    EXPECT_EQ(State(profile, Feature::Stars).effective, true);
    EXPECT_EQ(State(profile, Feature::Stars).change, Change::Forced);
    EXPECT_EQ(State(profile, Feature::Particles).effective, false);
    EXPECT_EQ(State(profile, Feature::Moon).change, Change::Unchanged);
    EXPECT_EQ(nlohmann::json::parse(profile.Manifest()).at("profile"), "explicit");
}

TEST(CaptureFeatureProfile, RefusesEmptyUnknownDuplicateOversizedAndConflictingRequests) {
    const CaptureFeatureContext context;
    const std::array<std::string_view, 1> unknown{"animals_typo"};
    const std::array<std::string_view, 1> empty{""};
    const std::array<std::string_view, 2> duplicate{"water", "water"};
    const std::array<std::string_view, 2> valid{"water", "stars"};
    const std::string long_name(65, 'a');
    const std::array<std::string_view, 1> overlong{long_name};
    const std::array<std::string_view, kCaptureFeatureCount + 1> too_many{};
    EXPECT_THROW((void)CaptureFeatureProfile::Resolve({}, {}, {}, context), std::invalid_argument);
    for (const auto names : {std::span<const std::string_view>(unknown),
                             std::span<const std::string_view>(empty),
                             std::span<const std::string_view>(duplicate),
                             std::span<const std::string_view>(overlong),
                             std::span<const std::string_view>(too_many)}) {
        EXPECT_THROW((void)CaptureFeatureProfile::Resolve({}, {}, {}, context, names),
                     std::invalid_argument);
    }
    EXPECT_THROW((void)CaptureFeatureProfile::Resolve(AllFeatures(), {}, {}, context, valid),
                 std::invalid_argument);
    EXPECT_THROW((void)CaptureFeatureName(Feature::Count), std::invalid_argument);
    EXPECT_THROW((void)CaptureFeatureName(static_cast<Feature>(-1)), std::invalid_argument);
}

TEST(CaptureFeatureProfile, ResolvesActualSystemGatesButNeverMistakesTuningFlagsForDisablement) {
    const auto systems = luminumbra::core::SystemConfig::FromJsonString(R"({
        "sim":{"hydrology_weather":{"enabled":false},"weather_events":{"enabled":false},
               "plant_growth":{"enabled":false},"ecology":{"enabled":false}},
        "render":{"live_weather":{"enabled":false},"creature_spawn":{"enabled":false},
                  "tree_wind":{"enabled":false},"plant_procgen":{"enabled":false},
                  "moonlight":{"enabled":false}}})");
    const auto profile = Resolve(AllFeatures(), EnabledContext(), systems);
    for (const auto feature :
         {Feature::WeatherHydrology, Feature::WeatherEvents, Feature::LiveWeatherBridge}) {
        EXPECT_EQ(State(profile, feature).effective, false);
        EXPECT_EQ(State(profile, feature).change, Change::Suppressed);
        EXPECT_EQ(State(profile, feature).capability, Capability::Refused);
    }
    for (const auto feature : {Feature::CreatureSpawning,
                               Feature::CreatureAI,
                               Feature::PlantGrowth,
                               Feature::PlantWind,
                               Feature::Foliage,
                               Feature::Moon}) {
        EXPECT_EQ(State(profile, feature).effective, true);
    }
    const auto unknown = Resolve(AllFeatures(), {}, systems);
    EXPECT_FALSE(State(unknown, Feature::CreatureSpawning).effective.has_value());
    EXPECT_EQ(State(unknown, Feature::WeatherHydrology).effective, false);
    EXPECT_EQ(profile.capability(), Capability::Refused);
}

TEST(CaptureFeatureProfile, RuntimeScenarioAndGraphicsSuppressionsAreExplicitAndDoNotMutateInputs) {
    auto runtime = AllFeatures();
    runtime.scenario = "world_visual_sweep";
    runtime.no_ui = true;
    runtime.hidden_window = true;
    luminumbra::core::GraphicsValues graphics;
    graphics.grass_enabled = false;
    const luminumbra::core::SystemConfig systems;
    const auto before = systems.ComputeConfigSubHash();
    const auto profile = Resolve(runtime, EnabledContext(), systems, graphics);
    EXPECT_EQ(State(profile, Feature::AmbientAnimals).reason,
              "scenario_suppresses_ambient_animals");
    EXPECT_EQ(State(profile, Feature::Foliage).reason, "graphics.grass_disabled");
    EXPECT_EQ(State(profile, Feature::Weather).effective, true);
    EXPECT_EQ(State(profile, Feature::Particles).effective, true);
    EXPECT_EQ(State(profile, Feature::CaveFlourishes).effective, true);
    EXPECT_EQ(runtime.scenario, "world_visual_sweep");
    EXPECT_FALSE(graphics.grass_enabled);
    EXPECT_EQ(systems.ComputeConfigSubHash(), before);
}

TEST(CaptureFeatureProfile, IsolationMasksEveryActualRenderConsumerAndBackdropReplacesSky) {
    auto runtime = AllFeatures();
    runtime.isolation_layers = "terrain";
    auto profile = Resolve(runtime, EnabledContext());
    for (const auto feature : {Feature::Water,
                               Feature::Rivers,
                               Feature::Foliage,
                               Feature::Particles,
                               Feature::AmbientParticles,
                               Feature::PrecipitationParticles,
                               Feature::WaterfallSpray,
                               Feature::Lightning}) {
        EXPECT_EQ(State(profile, feature).effective, false);
        EXPECT_EQ(State(profile, feature).reason, "isolation_layer_suppressed");
        EXPECT_EQ(State(profile, feature).capability, Capability::Refused);
    }
    EXPECT_EQ(State(profile, Feature::Sky).effective, true);
    EXPECT_EQ(State(profile, Feature::Waterfalls).effective, true);
    runtime.isolation_layers.clear();
    runtime.isolation_backdrop = "greenscreen";
    profile = Resolve(runtime, EnabledContext());
    for (const auto feature : {Feature::Sky, Feature::Moon, Feature::Stars, Feature::Clouds}) {
        EXPECT_EQ(State(profile, feature).effective, false);
        EXPECT_EQ(State(profile, feature).reason, "isolation_backdrop_replaces_sky");
    }
    // The shipping renderer does not consume its reserved skinned/sky layer bits.
    runtime.isolation_layers = "water,foliage,particles,lightning";
    runtime.isolation_backdrop = "scene";
    EXPECT_EQ(Resolve(runtime, EnabledContext()).capability(), Capability::Supported);
}

TEST(CaptureFeatureProfile, MissingIdentitiesPreventSupportAndReasonsSurviveSerialization) {
    for (const bool assets : {false, true}) {
        auto context = EnabledContext();
        auto& identity = assets ? context.asset_hash : context.config_hash;
        identity = {"", "unavailable\"reason\n"};
        const auto profile = Resolve(AllFeatures(), context);
        EXPECT_EQ(profile.capability(), Capability::Unsupported);
        const auto manifest = nlohmann::json::parse(profile.Manifest());
        EXPECT_EQ(manifest.at(assets ? "asset_hash" : "config_hash").at("unavailable"),
                  identity.unavailable);
    }
}

TEST(CaptureFeatureProfile, FinalForcedAndSuppressedObservationsOverrideBaselineControls) {
    auto runtime = AllFeatures();
    runtime.scenario = "world_visual_sweep";
    runtime.isolation_layers = "terrain";
    runtime.isolation_backdrop = "greenscreen";
    luminumbra::core::GraphicsValues graphics;
    graphics.grass_enabled = false;
    const auto systems = luminumbra::core::SystemConfig::FromJsonString(R"({
        "sim":{"hydrology_weather":{"enabled":false},"weather_events":{"enabled":false}},
        "render":{"live_weather":{"enabled":false}}})");
    for (const bool enabled : {true, false}) {
        auto context = EnabledContext();
        for (auto& adapter : context.adapters) {
            adapter = {.enabled = enabled,
                       .change = enabled ? Change::Forced : Change::Suppressed,
                       .reason = "final_mode_override"};
        }
        const auto profile = Resolve(runtime, context, systems, graphics);
        EXPECT_EQ(profile.capability(), enabled ? Capability::Supported : Capability::Refused);
        for (const auto& state : profile.features()) {
            EXPECT_EQ(state.effective, enabled);
            EXPECT_EQ(state.change, enabled ? Change::Forced : Change::Suppressed);
            EXPECT_EQ(state.reason, "final_mode_override");
        }
    }
}

TEST(CaptureFeatureProfile, RefusesInvalidEnumsBoundsIdentitiesAndAdapterStates) {
    auto runtime = AllFeatures();
    runtime.capture_feature_profile = static_cast<Selection>(-1);
    EXPECT_THROW((void)Resolve(runtime), std::invalid_argument);
    runtime = AllFeatures();
    runtime.isolation_layers.assign(1025, ' ');
    EXPECT_THROW((void)Resolve(runtime), std::invalid_argument);
    runtime = AllFeatures();
    runtime.isolation_backdrop.assign(257, ' ');
    EXPECT_THROW((void)Resolve(runtime), std::invalid_argument);
    auto context = EnabledContext();
    context.mode = static_cast<CaptureFeatureMode>(-1);
    EXPECT_THROW((void)Resolve(AllFeatures(), context), std::invalid_argument);
    for (const auto& identity : {CaptureIdentity{"", ""},
                                 CaptureIdentity{"hash", "also_unavailable"},
                                 CaptureIdentity{std::string(257, 'a'), ""},
                                 CaptureIdentity{"", std::string(257, 'a')},
                                 CaptureIdentity{std::string(1, static_cast<char>(0xff)), ""}}) {
        for (const bool assets : {false, true}) {
            context = EnabledContext();
            (assets ? context.asset_hash : context.config_hash) = identity;
            EXPECT_THROW((void)Resolve(AllFeatures(), context), std::invalid_argument);
        }
    }
    for (const auto& adapter :
         std::array{CaptureFeatureAdapter{true, Change::Unavailable, "unavailable"},
                    CaptureFeatureAdapter{std::nullopt, Change::Unchanged, "unknown"},
                    CaptureFeatureAdapter{false, Change::Unchanged, "mismatch"},
                    CaptureFeatureAdapter{false, Change::Forced, "contradiction"},
                    CaptureFeatureAdapter{true, Change::Suppressed, "contradiction"},
                    CaptureFeatureAdapter{std::nullopt, Change::Forced, "unknown"},
                    CaptureFeatureAdapter{std::nullopt, Change::Suppressed, "unknown"},
                    CaptureFeatureAdapter{true, static_cast<Change>(-1), "unknown_enum"},
                    CaptureFeatureAdapter{true, Change::Unchanged, ""},
                    CaptureFeatureAdapter{true, Change::Unchanged, std::string(257, 'x')},
                    CaptureFeatureAdapter{
                        true, Change::Unchanged, std::string(1, static_cast<char>(0xff))}}) {
        context = EnabledContext();
        Adapter(context, Feature::Water) = adapter;
        EXPECT_THROW((void)Resolve(AllFeatures(), context), std::invalid_argument);
    }
}

TEST(CaptureFeatureProfile, AcceptsExactBoundsAndOwnsSnapshotAfterResolution) {
    auto runtime = AllFeatures();
    runtime.isolation_layers.assign(1024, ' ');
    runtime.isolation_backdrop.assign(256, ' ');
    auto context = EnabledContext();
    context.asset_hash.value.assign(256, 'a');
    context.config_hash.value.assign(256, 'b');
    Adapter(context, Feature::Water).reason.assign(256, 'c');
    auto profile = Resolve(runtime, context);
    const auto before = profile.Manifest();
    Adapter(context, Feature::Water).enabled = false;
    EXPECT_EQ(profile.Manifest(), before);
    EXPECT_EQ(profile.capability(), Capability::Supported);
    profile = Resolve(AllFeatures(), EnabledContext());
    EXPECT_EQ(profile.capability(), Capability::Supported);
}

class FailingBuffer : public std::streambuf {
public:
    explicit FailingBuffer(bool fail_flush)
        : m_fail_flush(fail_flush) {}

protected:
    std::streamsize xsputn(const char*, std::streamsize count) override {
        return m_fail_flush ? count : count / 2;
    }
    int sync() override {
        return m_fail_flush ? -1 : 0;
    }

private:
    bool m_fail_flush;
};

class ThrowingBuffer : public std::streambuf {
public:
    explicit ThrowingBuffer(bool fail_flush)
        : m_fail_flush(fail_flush) {}

protected:
    std::streamsize xsputn(const char*, std::streamsize count) override {
        if (!m_fail_flush) {
            throw std::runtime_error("write failure");
        }
        return count;
    }
    int sync() override {
        throw std::runtime_error("flush failure");
    }

private:
    bool m_fail_flush;
};

TEST(CaptureFeatureProfile, StreamRefusalsIncludePartialWriteFailedFlushAndExceptions) {
    const auto profile = Resolve();
    for (const bool fail_flush : {false, true}) {
        for (const bool exceptions : {false, true}) {
            FailingBuffer buffer(fail_flush);
            std::ostream output(&buffer);
            if (exceptions) {
                output.exceptions(std::ios::failbit | std::ios::badbit);
            }
            EXPECT_FALSE(profile.WriteManifest(output));
            ThrowingBuffer throwing_buffer(fail_flush);
            std::ostream throwing_output(&throwing_buffer);
            if (exceptions) {
                throwing_output.exceptions(std::ios::failbit | std::ios::badbit);
            }
            EXPECT_FALSE(profile.WriteManifest(throwing_output));
        }
    }
    std::ostringstream already_failed;
    already_failed.setstate(std::ios::badbit);
    EXPECT_FALSE(profile.WriteManifest(already_failed));
}
} // namespace
