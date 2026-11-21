#include "../support/UniqueTempPath.h"
#include "core/RuntimeScenarioConfig.h"
#include "luminumbra_common/ai/CreatureBrainSystem.h"
#include "luminumbra_common/core/Environment.h"
#include "rendering/TemporalSweep.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace Luminumbra::Rendering;
namespace Harness = Luminumbra::Client::ScenarioHarness;
namespace Comp = Luminumbra::Components;
using Action = TemporalSequenceAction;

class ScopedSweepEnvironment {
public:
    explicit ScopedSweepEnvironment(const char* value)
        : previous_(Luminumbra::Core::ReadEnvironment("LUMINUMBRA_VISUAL_SWEEP")) {
        if (assign(value) != 0) {
            throw std::runtime_error("Cannot override sweep environment");
        }
    }
    ~ScopedSweepEnvironment() {
        EXPECT_EQ(assign(previous_ ? previous_->c_str() : nullptr), 0);
    }
    ScopedSweepEnvironment(const ScopedSweepEnvironment&) = delete;
    ScopedSweepEnvironment& operator=(const ScopedSweepEnvironment&) = delete;

private:
    static int assign(const char* value) {
#if defined(_WIN32)
        return _putenv_s("LUMINUMBRA_VISUAL_SWEEP", value ? value : "");
#else
        return value ? setenv("LUMINUMBRA_VISUAL_SWEEP", value, 1)
                     : unsetenv("LUMINUMBRA_VISUAL_SWEEP");
#endif
    }
    const std::optional<std::string> previous_;
};

CaptureFeatureProfile Profile(bool supported = true) {
    Harness::RuntimeScenarioConfig runtime;
    runtime.capture_feature_profile = Harness::RuntimeScenarioConfig::FeatureProfile::AllFeatures;
    CaptureFeatureContext context;
    context.mode = CaptureFeatureMode::WorldVisualSweep;
    context.asset_hash = {"resolved-assets", ""};
    context.config_hash = {"resolved-config", ""};
    if (supported) {
        for (auto& adapter : context.adapters) {
            adapter = {true, CaptureFeatureChange::Unchanged, "final_producer_enabled"};
        }
    }
    return CaptureFeatureProfile::Resolve(runtime, {}, {}, context);
}

TemporalAuditPlan Plan(TemporalAuditSpan span = TemporalAuditSpan::DayNight,
                       std::uint32_t rate = 7) {
    const auto admitted = BuildTemporalAuditPlan(
        {.span = span,
         .start_tick = 17,
         .ticks_per_day = 4ULL * rate,
         .ticks_per_second = rate,
         .dawn_tick = rate,
         .dusk_tick = 3ULL * rate,
         .strike_tick = span == TemporalAuditSpan::WeatherCycle ? 60ULL * rate : 0,
         .night_moon_phase_milli = 1000,
         .tick_limit = kTemporalMaxTicks,
         .frame_limit = kTemporalMaxFrames});
    EXPECT_TRUE(admitted.plan) << admitted.reason;
    return admitted.plan ? *admitted.plan : TemporalAuditPlan{};
}

TemporalSequenceBudget Budget(const TemporalAuditPlan& plan) {
    return {.frame_bytes = 1024,
            .manifest_bytes = 4096,
            .output_bytes = static_cast<std::uint64_t>(plan.frames.size()) * 1024 + 4096};
}

// Models only the transport boundary, not GPU presence. The actual creature
// brain runs on every requested tick; one long-lived particle's age is an oracle
// for continuity across every capture and frozen retry. At most 4096 snapshots.
struct Transport {
    explicit Transport(const TemporalAuditPlan& plan)
        : tick(plan.request.start_tick)
        , rate(plan.request.ticks_per_second) {
        subject = registry.create();
        registry.emplace<Comp::TransformComponent>(subject);
        registry.emplace<Comp::CreatureComponent>(subject).hunger = 0.5f;
        captures.reserve(plan.frames.size());
        ages.reserve(plan.frames.size());
        energies.reserve(plan.frames.size());
    }
    std::uint64_t actual_tick() {
        ++clock_queries;
        return tick;
    }
    bool advance_tick(const TemporalSequenceStep& command) {
        EXPECT_EQ(command.simulation_tick, tick + 1);
        luminumbra::ai::RunCreatureBrainSystemOnTick(registry, 1.0f / static_cast<float>(rate));
        ++tick;
        ++ticks;
        return failure != Action::AdvanceTick;
    }
    bool render(const TemporalSequenceStep& command,
                const CaptureFeaturePolicy& policy,
                const WorldSweepAngle& camera,
                bool& ready) {
        selected_camera = camera.label;
        EXPECT_FALSE(policy.UseLegacyAtmospherePins());
        EXPECT_EQ(policy.Apply({false, false, false, false, true}, false),
                  CaptureGameplayFeatures{});
        EXPECT_EQ(command.simulation_tick, tick);
        if (command.action == Action::Readiness || command.action == Action::InitialReadiness ||
            command.action == Action::Settle) {
            EXPECT_DOUBLE_EQ(command.presentation_delta_seconds, 0);
        }
        particle_age += command.presentation_delta_seconds;
        if (command.action == Action::InitialReadiness && retries > 0) {
            --retries;
            ready = false;
        }
        ++renders;
        return failure != command.action;
    }
    TemporalSweepOutput capture(const TemporalSequenceStep& command) {
        captures.push_back(command);
        ages.push_back(particle_age);
        energies.push_back(registry.get<Comp::CreatureComponent>(subject).energy);
        return {failure != Action::Capture, empty_output ? 0 : command.byte_limit};
    }
    TemporalSweepOutput publish(const TemporalSequenceStep& command) {
        ++publications;
        return {failure != Action::Publish, empty_output ? 0 : command.byte_limit};
    }

    entt::registry registry;
    entt::entity subject = entt::null;
    std::uint64_t tick;
    std::uint32_t rate;
    std::uint64_t ticks = 0;
    std::uint64_t renders = 0;
    std::uint64_t clock_queries = 0;
    unsigned publications = 0;
    unsigned retries = 0;
    Action failure = Action::Refused;
    bool empty_output = false;
    double particle_age = 0;
    std::string_view selected_camera;
    std::vector<TemporalSequenceStep> captures;
    std::vector<double> ages;
    std::vector<float> energies;
};

TEST(TemporalSweep, ExactTickPlanAcrossEverySpanAndRationalCadence) {
    for (const auto span : {TemporalAuditSpan::DayNight,
                            TemporalAuditSpan::WeatherCycle,
                            TemporalAuditSpan::MultiDay}) {
        for (const auto rate : {1U, 7U, 30U, 120U}) {
            const auto plan = Plan(span, rate);
            Transport transport(plan);
            EXPECT_EQ(RunTemporalSweep(plan, Budget(plan), Profile(), 0, transport), "");
            ASSERT_EQ(transport.captures.size(), plan.frames.size());
            for (std::size_t index = 0; index < plan.frames.size(); ++index) {
                EXPECT_EQ(transport.captures[index].simulation_tick,
                          plan.frames[index].target_tick);
                EXPECT_EQ(transport.captures[index].sub_tick_60, plan.frames[index].sub_tick_60);
                EXPECT_EQ(transport.captures[index].control, plan.frames[index].control);
                EXPECT_EQ(transport.captures[index].frame_index, index);
            }
            EXPECT_EQ(transport.ticks, plan.end_tick - plan.request.start_tick);
            EXPECT_EQ(transport.tick, plan.end_tick);
            EXPECT_EQ(transport.publications, 1U);
        }
    }
}

TEST(TemporalSweep, CreatureProgressionCarriesAcrossCellsAndMatchesOrdinaryTicks) {
    const auto plan = Plan(TemporalAuditSpan::MultiDay);
    Transport transport(plan);
    Transport ordinary(plan);
    ASSERT_EQ(RunTemporalSweep(plan, Budget(plan), Profile(), 3, transport), "");
    for (auto tick = plan.request.start_tick; tick < plan.end_tick; ++tick) {
        luminumbra::ai::RunCreatureBrainSystemOnTick(ordinary.registry,
                                                     1.0f / static_cast<float>(ordinary.rate));
    }
    const auto& actual = transport.registry.get<Comp::CreatureComponent>(transport.subject);
    const auto& expected = ordinary.registry.get<Comp::CreatureComponent>(ordinary.subject);
    EXPECT_EQ(actual.energy, expected.energy);
    EXPECT_EQ(actual.hunger, expected.hunger);
    EXPECT_EQ(actual.stamina, expected.stamina);
    EXPECT_EQ(actual.last_action, expected.last_action);
    ASSERT_GT(transport.energies.size(), 1U);
    EXPECT_NE(transport.energies.front(), transport.energies.back());
}

TEST(TemporalSweep, ParticleContinuityCarriesAcrossCellsAndRetriesDoNotAge) {
    const auto plan = Plan();
    Transport transport(plan);
    Transport retrying(plan);
    retrying.retries = 2;
    ASSERT_EQ(RunTemporalSweep(plan, Budget(plan), Profile(), 0, transport), "");
    ASSERT_EQ(RunTemporalSweep(plan, Budget(plan), Profile(), 0, retrying), "");
    EXPECT_EQ(transport.ages, retrying.ages);
    EXPECT_EQ(transport.captures, retrying.captures);
    EXPECT_EQ(retrying.renders, transport.renders + 2);
    ASSERT_EQ(transport.ages.size(), plan.frames.size());
    ASSERT_GT(transport.ages.size(), 1U);
    EXPECT_GT(transport.ages.front(), 0);
    for (std::size_t index = 1; index < transport.ages.size(); ++index) {
        EXPECT_GE(transport.ages[index], transport.ages[index - 1]);
    }
    EXPECT_GT(transport.ages.back(), transport.ages.front());
}

TEST(TemporalSweep, IncompatibleCameraAndUnavailableProfileRefuseBeforeTransport) {
    const auto plan = Plan();
    for (const bool supported : {false, true}) {
        for (const std::size_t camera : {0U, 4U, 6U}) {
            if (supported && camera == 0) {
                continue;
            }
            Transport transport(plan);
            const auto expected = camera == 6   ? "sweep_unknown_camera"
                                  : camera == 4 ? kWorldSweepFoliageRefusal
                                                : "sweep_feature_profile_unavailable";
            EXPECT_EQ(RunTemporalSweep(plan, Budget(plan), Profile(supported), camera, transport),
                      expected);
            EXPECT_EQ(transport.clock_queries, 0U);
            EXPECT_EQ(transport.ticks, 0U);
            EXPECT_EQ(transport.renders, 0U);
            EXPECT_TRUE(transport.captures.empty());
            EXPECT_EQ(transport.publications, 0U);
        }
    }
}

TEST(TemporalSweep, SupportedCamerasReceiveTheirPinsAndIncompleteProfilesRefuse) {
    const auto plan = Plan();
    for (const std::size_t camera : {0U, 1U, 2U, 3U, 5U}) {
        Transport transport(plan);
        EXPECT_EQ(RunTemporalSweep(plan, Budget(plan), Profile(), camera, transport), "");
        EXPECT_EQ(transport.selected_camera, kWorldSweepAngles[camera].label);
    }
    CaptureFeatureContext context;
    context.mode = CaptureFeatureMode::WorldVisualSweep;
    context.asset_hash = {"resolved-assets", ""};
    context.config_hash = {"resolved-config", ""};
    for (auto& adapter : context.adapters) {
        adapter = {true, CaptureFeatureChange::Unchanged, "final_producer_enabled"};
    }
    const std::array<std::string_view, 1> requested{{"particles"}};
    const auto subset = CaptureFeatureProfile::Resolve({}, {}, {}, context, requested);
    EXPECT_EQ(subset.capability(), CaptureFeatureCapability::Supported);
    EXPECT_EQ(TemporalSweepRefusal(subset, 0), "sweep_feature_profile_incomplete");
    Harness::RuntimeScenarioConfig runtime;
    runtime.capture_feature_profile = Harness::RuntimeScenarioConfig::FeatureProfile::AllFeatures;
    context.adapters[static_cast<std::size_t>(CaptureFeature::Particles)] = {
        false, CaptureFeatureChange::Suppressed, "final_particles_disabled"};
    const auto refused = CaptureFeatureProfile::Resolve(runtime, {}, {}, context);
    EXPECT_EQ(refused.capability(), CaptureFeatureCapability::Refused);
    EXPECT_EQ(TemporalSweepRefusal(refused, 0), "sweep_feature_profile_unavailable");
}

TEST(TemporalSweep, PlanBudgetClockAndTransportFailuresNeverPublishSuccess) {
    const auto plan = Plan();
    auto altered = plan;
    altered.frames.clear();
    Transport invalid_plan(plan);
    EXPECT_NE(RunTemporalSweep(altered, Budget(plan), Profile(), 0, invalid_plan), "");
    EXPECT_EQ(invalid_plan.clock_queries, 0U);
    Transport invalid_budget(plan);
    auto budget = Budget(plan);
    --budget.output_bytes;
    EXPECT_NE(RunTemporalSweep(plan, budget, Profile(), 0, invalid_budget), "");
    EXPECT_EQ(invalid_budget.clock_queries, 0U);
    Transport invalid_clock(plan);
    ++invalid_clock.tick;
    EXPECT_NE(RunTemporalSweep(plan, Budget(plan), Profile(), 0, invalid_clock), "");
    EXPECT_EQ(invalid_clock.renders, 0U);
    for (const auto failure : {Action::Warmup,
                               Action::InitialReadiness,
                               Action::AdvanceTick,
                               Action::Present,
                               Action::Settle,
                               Action::Readiness,
                               Action::Capture,
                               Action::Publish}) {
        Transport transport(plan);
        transport.failure = failure;
        EXPECT_NE(RunTemporalSweep(plan, Budget(plan), Profile(), 0, transport), "");
        EXPECT_EQ(transport.publications, failure == Action::Publish ? 1U : 0U);
    }
    Transport empty(plan);
    empty.empty_output = true;
    EXPECT_NE(RunTemporalSweep(plan, Budget(plan), Profile(), 0, empty), "");
    EXPECT_EQ(empty.publications, 0U);
    Transport not_ready(plan);
    not_ready.retries = plan.settling.initial_readiness_retries;
    EXPECT_EQ(RunTemporalSweep(plan, Budget(plan), Profile(), 0, not_ready),
              "sequence readiness exhausted");
    EXPECT_TRUE(not_ready.captures.empty());
}

TEST(TemporalSweep, LegacyStillMatrixPreservesEveryPinAndCellOrder) {
    constexpr std::array<const char*, 4> times{{"dawn", "noon", "dusk", "night"}};
    constexpr std::array<float, 4> tods{{0.17f, 0.0f, 0.25f, 0.5f}};
    constexpr std::array<const char*, 6> angles{
        {"yaw000", "yaw120", "yaw240", "down35", "up25", "water"}};
    constexpr std::array<float, 6> yaws{{0.0f, 120.0f, 240.0f, 45.0f, 200.0f, 0.0f}};
    constexpr std::array<float, 6> pitches{{0.0f, 0.0f, 0.0f, -35.0f, 25.0f, 0.0f}};
    constexpr std::array<const char*, 6> features{{"horizon_vista",
                                                   "horizon_vista",
                                                   "horizon_vista",
                                                   "ground_foliage",
                                                   "sky_clouds",
                                                   "water_shore"}};
    ASSERT_EQ(kWorldSweepTimes.size(), times.size());
    ASSERT_EQ(kWorldSweepAngles.size(), angles.size());
    ASSERT_EQ(kWorldSweepWeather.size(), 2U);
    EXPECT_STREQ(kWorldSweepWeather[0], "clear");
    EXPECT_STREQ(kWorldSweepWeather[1], "storm");
    std::size_t cells = 0;
    for (std::size_t time = 0; time < times.size(); ++time) {
        EXPECT_STREQ(kWorldSweepTimes[time].label, times[time]);
        EXPECT_EQ(kWorldSweepTimes[time].time_of_day, tods[time]);
        for (const bool storm : {false, true}) {
            EXPECT_EQ(WorldSweepStillSettleFrames(storm), storm ? 5 : 3);
            for (std::size_t angle = 0; angle < angles.size(); ++angle) {
                const auto& actual = kWorldSweepAngles[angle];
                EXPECT_STREQ(actual.label, angles[angle]);
                EXPECT_EQ(actual.yaw_degrees, yaws[angle]);
                EXPECT_EQ(actual.pitch_degrees, pitches[angle]);
                EXPECT_EQ(actual.aim_at_water, angle == 5);
                EXPECT_STREQ(actual.feature, features[angle]);
                EXPECT_EQ(WorldSweepClearsFoliage(actual.pitch_degrees), angle == 4);
                ++cells;
            }
        }
    }
    EXPECT_EQ(cells, 48U);
    EXPECT_FALSE(WorldSweepClearsFoliage(5.0f));
    EXPECT_TRUE(WorldSweepClearsFoliage(5.01f));
    EXPECT_EQ(WorldSweepMatrixRefusal(false), "");
}

TEST(TemporalSweep, BothLaunchParsersRefuseIncompatibleMatrixBeforeAnyOutput) {
    const auto directory = Luminumbra::TestSupport::CreateUniqueTempDirectory("sweep-");
    const auto save_root = directory / "uncreated";
    for (const bool environment : {false, true}) {
        ScopedSweepEnvironment override(environment ? "1" : nullptr);
        for (const bool assigned : {false, true}) {
            std::vector<std::string> arguments{"client", "--save-root", save_root.string()};
            if (!environment) {
                arguments.push_back("--scenario=world_visual_sweep");
            }
            if (assigned) {
                arguments.push_back("--capture-feature-profile=all_features");
            } else {
                arguments.insert(arguments.end(), {"--capture-feature-profile", "all_features"});
            }
            std::vector<char*> argv;
            for (auto& argument : arguments) {
                argv.push_back(argument.data());
            }
            const auto argc = static_cast<int>(argv.size());
            for (const bool paths : {false, true}) {
                try {
                    if (paths) {
                        (void)Harness::ParseRuntimePathOptions(argc, argv.data());
                    } else {
                        (void)Harness::ParseRuntimeScenarioConfig(argc, argv.data(), directory);
                    }
                    FAIL() << "incompatible matrix admitted";
                } catch (const std::invalid_argument& error) {
                    EXPECT_EQ(error.what(), kWorldSweepFoliageRefusal);
                }
                EXPECT_FALSE(std::filesystem::exists(save_root));
            }
        }
    }
    std::filesystem::remove_all(directory);
}

TEST(TemporalSweep, LegacyLaunchAndExplicitScenarioOverrideStayAdmitted) {
    const auto directory = Luminumbra::TestSupport::CreateUniqueTempDirectory("sweep-legacy-");
    for (const auto* environment : std::array<const char*, 4>{{nullptr, "", "0", "1"}}) {
        ScopedSweepEnvironment override(environment);
        for (const bool other_scenario : {false, true}) {
            std::vector<std::string> arguments{"client"};
            if (other_scenario) {
                arguments.insert(
                    arguments.end(),
                    {"--scenario=skybox_visual_smoke", "--capture-feature-profile=all_features"});
            } else if (environment == nullptr) {
                arguments.push_back("--scenario=world_visual_sweep");
            }
            std::vector<char*> argv;
            for (auto& argument : arguments) {
                argv.push_back(argument.data());
            }
            const auto argc = static_cast<int>(argv.size());
            EXPECT_NO_THROW((void)Harness::ParseRuntimePathOptions(argc, argv.data()));
            const auto config = Harness::ParseRuntimeScenarioConfig(argc, argv.data(), directory);
            if (other_scenario) {
                EXPECT_EQ(config.scenario, "skybox_visual_smoke");
            } else {
                EXPECT_EQ(config.capture_feature_profile,
                          Harness::RuntimeScenarioConfig::FeatureProfile::None);
                EXPECT_EQ(config.world_visual_sweep(),
                          environment == nullptr || std::string_view(environment) == "1");
            }
        }
    }
    std::filesystem::remove_all(directory);
}
} // namespace
