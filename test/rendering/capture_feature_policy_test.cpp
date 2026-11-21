#include "core/RuntimeScenarioConfig.h"
#include "rendering/CaptureFeatureProfile.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>

namespace {
using namespace Luminumbra::Rendering;
using Luminumbra::Client::ScenarioHarness::RuntimeScenarioConfig;

constexpr std::array kModes{CaptureFeatureMode::Timelapse,
                            CaptureFeatureMode::FrameScan,
                            CaptureFeatureMode::Scenario,
                            CaptureFeatureMode::TimeOfDaySweep,
                            CaptureFeatureMode::WeatherVisual,
                            CaptureFeatureMode::HeadlessScan,
                            CaptureFeatureMode::HeadlessScene,
                            CaptureFeatureMode::WorldVisualSweep};

CaptureFeatureContext SupportedContext(CaptureFeatureMode mode) {
    CaptureFeatureContext context;
    context.mode = mode;
    context.asset_hash = {"resolved-assets", ""};
    context.config_hash = {"resolved-config", ""};
    for (auto& adapter : context.adapters) {
        adapter = {.enabled = true,
                   .change = CaptureFeatureChange::Unchanged,
                   .reason = "ordinary_play_setting"};
    }
    return context;
}

RuntimeScenarioConfig AllFeatures() {
    RuntimeScenarioConfig runtime;
    runtime.capture_feature_profile = RuntimeScenarioConfig::FeatureProfile::AllFeatures;
    return runtime;
}

CaptureGameplayFeatures LegacyState(unsigned int bits) {
    return {.ambient_animals = (bits & 1u) != 0,
            .weather = (bits & 2u) != 0,
            .particles = (bits & 4u) != 0,
            .cave_flourishes = (bits & 8u) != 0,
            .ui = (bits & 16u) != 0};
}

TEST(CaptureFeaturePolicy, CompleteSupportedProfileEqualsOrdinaryPlayForEveryMode) {
    const CaptureGameplayFeatures ordinary_play;
    EXPECT_TRUE(ordinary_play.ambient_animals);
    EXPECT_TRUE(ordinary_play.weather);
    EXPECT_TRUE(ordinary_play.particles);
    EXPECT_TRUE(ordinary_play.cave_flourishes);
    for (const auto mode : kModes) {
        SCOPED_TRACE(static_cast<int>(mode));
        const auto profile =
            CaptureFeatureProfile::Resolve(AllFeatures(), {}, {}, SupportedContext(mode));
        ASSERT_EQ(profile.capability(), CaptureFeatureCapability::Supported);
        const auto policy = CaptureFeaturePolicy::Resolve(&profile);
        const CaptureGameplayFeatures suppressed{false, false, false, false, true};
        EXPECT_EQ(policy.Apply(suppressed, false), ordinary_play);
        EXPECT_EQ(policy.Apply(ordinary_play, false), ordinary_play);
        EXPECT_FALSE(policy.UseLegacyAtmospherePins());
    }
}

TEST(CaptureFeaturePolicy, AbsentAndUnimplementedProfilesPreserveEveryLegacyState) {
    for (const auto mode : kModes) {
        SCOPED_TRACE(static_cast<int>(mode));
        CaptureFeatureContext context;
        context.mode = mode;
        const auto unsupported = CaptureFeatureProfile::Resolve(AllFeatures(), {}, {}, context);
        ASSERT_EQ(unsupported.capability(), CaptureFeatureCapability::Unsupported);
        for (const auto* profile :
             {static_cast<const CaptureFeatureProfile*>(nullptr), &unsupported}) {
            const auto policy = CaptureFeaturePolicy::Resolve(profile);
            EXPECT_TRUE(policy.UseLegacyAtmospherePins());
            // All combinations of the five legacy settings, not just defaults.
            for (unsigned int bits = 0; bits < 32u; ++bits) {
                const auto legacy = LegacyState(bits);
                EXPECT_EQ(policy.Apply(legacy, false), legacy);
                auto without_ui = legacy;
                without_ui.ui = false;
                EXPECT_EQ(policy.Apply(legacy, true), without_ui);
            }
        }
    }
}

TEST(CaptureFeaturePolicy, MissingIdentitiesAdaptersAndEveryDisabledFeatureCannotAdmit) {
    for (const auto mode : kModes) {
        for (std::size_t index = 0; index < kCaptureFeatureCount; ++index) {
            SCOPED_TRACE(static_cast<int>(mode));
            SCOPED_TRACE(index);
            for (const bool unavailable : {false, true}) {
                auto context = SupportedContext(mode);
                context.adapters[index] =
                    unavailable ? CaptureFeatureAdapter{}
                                : CaptureFeatureAdapter{
                                      false, CaptureFeatureChange::Suppressed, "fixture_disabled"};
                const auto profile = CaptureFeatureProfile::Resolve(AllFeatures(), {}, {}, context);
                const auto policy = CaptureFeaturePolicy::Resolve(&profile);
                const CaptureGameplayFeatures legacy{false, false, false, false, false};
                EXPECT_TRUE(policy.UseLegacyAtmospherePins());
                EXPECT_EQ(policy.Apply(legacy, false), legacy);
            }
        }
        for (const bool missing_assets : {false, true}) {
            auto context = SupportedContext(mode);
            (missing_assets ? context.asset_hash : context.config_hash) = {"",
                                                                           "identity_unavailable"};
            const auto profile = CaptureFeatureProfile::Resolve(AllFeatures(), {}, {}, context);
            EXPECT_TRUE(CaptureFeaturePolicy::Resolve(&profile).UseLegacyAtmospherePins());
        }
    }
}

TEST(CaptureFeaturePolicy, SupportedSubsetsCannotAuthorizeUnrequestedProducers) {
    auto context = SupportedContext(CaptureFeatureMode::Scenario);
    for (std::size_t omitted = 0; omitted < kCaptureFeatureCount; ++omitted) {
        std::array<std::string_view, kCaptureFeatureCount - 1> requested{};
        std::size_t next = 0;
        for (std::size_t index = 0; index < kCaptureFeatureCount; ++index) {
            if (index != omitted) {
                requested[next++] = CaptureFeatureName(static_cast<CaptureFeature>(index));
            }
        }
        const auto profile = CaptureFeatureProfile::Resolve({}, {}, {}, context, requested);
        ASSERT_EQ(profile.capability(), CaptureFeatureCapability::Supported);
        EXPECT_TRUE(CaptureFeaturePolicy::Resolve(&profile).UseLegacyAtmospherePins());
    }
}

TEST(CaptureFeaturePolicy, ResolvedSystemGraphicsAndIsolationDisablementsRetainLegacyPolicy) {
    const auto context = SupportedContext(CaptureFeatureMode::HeadlessScene);
    const CaptureGameplayFeatures legacy{false, false, false, false, false};
    for (const bool backdrop : {false, true}) {
        auto runtime = AllFeatures();
        if (backdrop) {
            runtime.isolation_backdrop = "greenscreen";
        } else {
            runtime.isolation_layers = "terrain";
        }
        const auto profile = CaptureFeatureProfile::Resolve(runtime, {}, {}, context);
        EXPECT_EQ(CaptureFeaturePolicy::Resolve(&profile).Apply(legacy, false), legacy);
        EXPECT_TRUE(CaptureFeaturePolicy::Resolve(&profile).UseLegacyAtmospherePins());
    }
    luminumbra::core::GraphicsValues graphics;
    graphics.grass_enabled = false;
    const auto graphics_profile =
        CaptureFeatureProfile::Resolve(AllFeatures(), {}, graphics, context);
    EXPECT_EQ(CaptureFeaturePolicy::Resolve(&graphics_profile).Apply(legacy, false), legacy);
    const auto systems = luminumbra::core::SystemConfig::FromJsonString(
        R"({"sim":{"weather_events":{"enabled":false}}})");
    const auto system_profile = CaptureFeatureProfile::Resolve(AllFeatures(), systems, {}, context);
    EXPECT_EQ(CaptureFeaturePolicy::Resolve(&system_profile).Apply(legacy, false), legacy);
}

TEST(CaptureFeaturePolicy, UiAndDeterminismRestrictionsComposeWithoutReenablingSettings) {
    for (const auto mode : kModes) {
        const auto profile =
            CaptureFeatureProfile::Resolve(AllFeatures(), {}, {}, SupportedContext(mode));
        for (const bool determinism_fixture : {false, true}) {
            const auto policy = CaptureFeaturePolicy::Resolve(&profile, determinism_fixture);
            EXPECT_EQ(policy.UseLegacyAtmospherePins(), determinism_fixture);
            for (unsigned int bits = 0; bits < 32u; ++bits) {
                const auto legacy = LegacyState(bits);
                for (const bool no_ui : {false, true}) {
                    auto expected = determinism_fixture ? legacy : CaptureGameplayFeatures{};
                    expected.ui = legacy.ui && !no_ui;
                    EXPECT_EQ(policy.Apply(legacy, no_ui), expected);
                }
            }
        }
    }
}

TEST(CaptureFeaturePolicy, ScenarioSuppressionRequiresAnExplicitFinalProducerObservation) {
    auto runtime = AllFeatures();
    runtime.scenario = "weather_visual_smoke";
    auto context = SupportedContext(CaptureFeatureMode::WeatherVisual);
    auto profile = CaptureFeatureProfile::Resolve(runtime, {}, {}, context);
    EXPECT_EQ(profile.capability(), CaptureFeatureCapability::Refused);
    EXPECT_TRUE(CaptureFeaturePolicy::Resolve(&profile).UseLegacyAtmospherePins());
    context.adapters[static_cast<std::size_t>(CaptureFeature::AmbientAnimals)] = {
        true, CaptureFeatureChange::Forced, "final_ambient_animals_enabled"};
    profile = CaptureFeatureProfile::Resolve(runtime, {}, {}, context);
    EXPECT_EQ(profile.capability(), CaptureFeatureCapability::Supported);
    EXPECT_FALSE(CaptureFeaturePolicy::Resolve(&profile).UseLegacyAtmospherePins());
}
} // namespace
