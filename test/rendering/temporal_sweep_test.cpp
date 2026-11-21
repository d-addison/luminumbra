#include "../support/UniqueTempPath.h"
#include "core/RuntimeScenarioConfig.h"
#include "luminumbra_common/ai/CreatureBrainSystem.h"
#include "luminumbra_common/core/Environment.h"
#include "luminumbra_common/systems/WeatherSystem.h"
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

// Exercise the same statically dispatched runtime seam used by the GL owner.
// The creature brain is real; completed RGB transport and particles are modeled.
struct RuntimeBackend {
    enum class Fault {
        None,
        Begin,
        Simulation,
        WeatherTick,
        WeatherRender,
        Render,
        NotReady,
        Capture,
        EmptyCapture,
        Publish,
        EmptyPublish
    };
    explicit RuntimeBackend(const TemporalAuditPlan& plan)
        : subject(plan)
        , weather_tick(plan.request.start_tick) {}
    bool begin(const TemporalAuditPlan&, const TemporalSequenceBudget&) {
        ++begins;
        return fault != Fault::Begin;
    }
    std::uint64_t actual_tick() const {
        return subject.tick;
    }
    bool advance_simulation(const TemporalSequenceStep& step) {
        if (fault == Fault::Simulation) {
            return false;
        }
        const bool success = subject.advance_tick(step);
        if (fault != Fault::WeatherTick) {
            weather_tick = subject.tick;
        }
        return success;
    }
    bool weather_ready(std::uint64_t tick) const {
        return weather_tick == tick;
    }
    bool apply_weather(const TemporalSequenceStep& step) {
        EXPECT_EQ(step.simulation_tick, weather_tick);
        ++weather_samples;
        return fault != Fault::WeatherRender;
    }
    bool render_completed(const TemporalSequenceStep& step,
                          const CaptureFeaturePolicy& policy,
                          const WorldSweepAngle& camera,
                          bool& ready) {
        EXPECT_TRUE(policy.UseLegacyAtmospherePins());
        EXPECT_STREQ(camera.label, "yaw000");
        EXPECT_EQ(step.simulation_tick, subject.tick);
        ++subject.renders;
        subject.particle_age += step.presentation_delta_seconds;
        ready = fault != Fault::NotReady;
        if (step.action == Action::InitialReadiness && subject.retries > 0) {
            --subject.retries;
            ready = false;
        }
        if (step.action == Action::Readiness || step.action == Action::Settle ||
            step.action == Action::InitialReadiness) {
            EXPECT_DOUBLE_EQ(step.presentation_delta_seconds, 0);
        }
        completed = ready ? std::optional(step) : std::nullopt;
        return fault != Fault::Render;
    }
    TemporalSweepOutput capture(const TemporalSequenceStep& step) {
        EXPECT_TRUE(completed);
        if (completed) {
            EXPECT_EQ(completed->simulation_tick, step.simulation_tick);
            EXPECT_EQ(completed->sub_tick_60, step.sub_tick_60);
            EXPECT_EQ(completed->frame_index, step.frame_index);
            EXPECT_EQ(completed->control, step.control);
        }
        if (subject.captures.size() == 1 && fault == Fault::Capture) {
            return {};
        }
        if (fault == Fault::EmptyCapture) {
            return {true, 0};
        }
        return subject.capture(step);
    }
    TemporalSweepOutput publish(const TemporalSequenceStep& step) {
        ++publish_attempts;
        if (fault == Fault::Publish) {
            return {};
        }
        if (fault == Fault::EmptyPublish) {
            return {true, 0};
        }
        return subject.publish(step);
    }
    Transport subject;
    std::uint64_t weather_tick;
    unsigned begins = 0;
    unsigned publish_attempts = 0;
    std::uint64_t weather_samples = 0;
    Fault fault = Fault::None;
    std::optional<TemporalSequenceStep> completed;
};

// Compile-only mutation oracles for the runtime ordering and sticky failure gates.
struct RuntimeWireProbe {
    std::uint64_t tick = 0;
    bool simulation_ok = true;
    bool weather_ok = true;
    bool sample_ok = true;
    bool render_ok = true;
    unsigned samples = 0;
    unsigned renders = 0;
    constexpr bool advance_simulation(const TemporalSequenceStep& step) {
        if (simulation_ok) {
            tick = step.simulation_tick;
        }
        return simulation_ok;
    }
    constexpr bool weather_ready(std::uint64_t expected) const {
        return weather_ok && tick == expected;
    }
    constexpr bool apply_weather(const TemporalSequenceStep&) {
        ++samples;
        return sample_ok;
    }
    constexpr bool render_completed(const TemporalSequenceStep&,
                                    const CaptureFeaturePolicy&,
                                    const WorldSweepAngle&,
                                    bool& ready) {
        ++renders;
        ready = false;
        return render_ok;
    }
};
constexpr bool RuntimeWireContract() {
    const TemporalSequenceStep step{.action = Action::AdvanceTick, .simulation_tick = 1};
    const CaptureFeaturePolicy policy;
    RuntimeWireProbe backend;
    Harness::WorldVisualTemporalTransport transport(backend);
    bool ready = true;
    if (!transport.advance_tick(step) || backend.tick != 1 ||
        !transport.render(step, policy, kWorldSweepAngles[0], ready) || ready ||
        backend.samples != 1 || backend.renders != 1) {
        return false;
    }
    backend.simulation_ok = false;
    if (transport.advance_tick(step)) {
        return false;
    }
    backend.simulation_ok = true;
    backend.weather_ok = false;
    if (transport.advance_tick(step) ||
        transport.render(step, policy, kWorldSweepAngles[0], ready) || backend.samples != 1 ||
        backend.renders != 1) {
        return false;
    }
    backend.weather_ok = true;
    backend.sample_ok = false;
    if (transport.render(step, policy, kWorldSweepAngles[0], ready) || backend.renders != 1) {
        return false;
    }
    backend.sample_ok = true;
    backend.render_ok = false;
    return !transport.render(step, policy, kWorldSweepAngles[0], ready);
}
static_assert(RuntimeWireContract());

constexpr auto kRuntimeClockRequest = TemporalAuditRequest{.start_tick = 0,
                                                           .ticks_per_day = 162000,
                                                           .ticks_per_second = 30,
                                                           .dawn_tick = 27000,
                                                           .dusk_tick = 135000};
// Compile-time oracles permit mutation checking without executing native tests.
static_assert(Harness::TemporalSweepClockMatches(kRuntimeClockRequest, 0, 30, {}));
static_assert(!Harness::TemporalSweepClockMatches(kRuntimeClockRequest, 1, 30, {}));
static_assert(!Harness::TemporalSweepClockMatches(kRuntimeClockRequest, 0, 7, {}));
static_assert(!Harness::TemporalSweepClockMatches(
    kRuntimeClockRequest, 0, 30, Luminumbra::world::WorldCalendar::Legacy()));

TEST(TemporalSweepRuntime, ResolvedClockAdmissionRejectsEveryMismatch) {
    const auto matches = [](const TemporalAuditRequest& request,
                            const Luminumbra::world::WorldCalendar& calendar) {
        return Harness::TemporalSweepClockMatches(request, 0, 30, calendar);
    };
    EXPECT_TRUE(matches(kRuntimeClockRequest, {}));
    for (const auto field : {0, 1, 2, 3, 4}) {
        auto request = kRuntimeClockRequest;
        switch (field) {
            case 0:
                ++request.start_tick;
                break;
            case 1:
                ++request.ticks_per_second;
                break;
            case 2:
                ++request.ticks_per_day;
                break;
            case 3:
                ++request.dawn_tick;
                break;
            default:
                ++request.dusk_tick;
                break;
        }
        EXPECT_FALSE(matches(request, {}));
    }
    for (const auto field : {0, 1, 2, 3, 4, 5}) {
        auto calendar = Luminumbra::world::WorldCalendar{};
        switch (field) {
            case 0:
                calendar.dayLengthTicks = 0;
                break;
            case 1:
                calendar.dayLengthTicks = 1ULL << 31;
                break;
            case 2:
                calendar.daylightDenominator = 0;
                break;
            case 3:
                calendar.daylightDenominator = 65536;
                break;
            case 4:
                calendar.daylightNumerator = 0;
                break;
            default:
                calendar.daylightNumerator = calendar.daylightDenominator;
                break;
        }
        EXPECT_FALSE(matches(kRuntimeClockRequest, calendar));
    }
}

TEST(TemporalSweepRuntime, ExactTicksWeatherAndCompletedFramesCarryAcrossCells) {
    for (const auto span : {TemporalAuditSpan::DayNight,
                            TemporalAuditSpan::WeatherCycle,
                            TemporalAuditSpan::MultiDay}) {
        const auto plan = Plan(span, 30);
        RuntimeBackend runtime(plan);
        RuntimeBackend ordinary(plan);
        runtime.subject.retries = 2;
        ASSERT_EQ(Harness::RunWorldVisualTemporalSweep(plan, Budget(plan), runtime), "");
        ASSERT_EQ(runtime.begins, 1U);
        ASSERT_EQ(runtime.subject.captures.size(), plan.frames.size());
        EXPECT_EQ(runtime.subject.ticks, plan.end_tick - plan.request.start_tick);
        EXPECT_EQ(runtime.weather_tick, plan.end_tick);
        EXPECT_EQ(runtime.weather_samples, runtime.subject.renders);
        EXPECT_EQ(runtime.subject.publications, 1U);
        for (auto tick = plan.request.start_tick; tick < plan.end_tick; ++tick) {
            luminumbra::ai::RunCreatureBrainSystemOnTick(ordinary.subject.registry, 1.0f / 30.0f);
        }
        EXPECT_EQ(
            runtime.subject.registry.get<Comp::CreatureComponent>(runtime.subject.subject).energy,
            ordinary.subject.registry.get<Comp::CreatureComponent>(ordinary.subject.subject)
                .energy);
        RuntimeBackend without_retries(plan);
        ASSERT_EQ(Harness::RunWorldVisualTemporalSweep(plan, Budget(plan), without_retries), "");
        EXPECT_EQ(runtime.subject.ages, without_retries.subject.ages);
        EXPECT_GT(runtime.subject.ages.back(), runtime.subject.ages.front());
        for (std::size_t index = 0; index < plan.frames.size(); ++index) {
            EXPECT_EQ(runtime.subject.captures[index].simulation_tick,
                      plan.frames[index].target_tick);
            EXPECT_EQ(runtime.subject.captures[index].sub_tick_60, plan.frames[index].sub_tick_60);
            EXPECT_EQ(runtime.subject.captures[index].control, plan.frames[index].control);
        }
    }
}

TEST(TemporalSweepRuntime, EveryRuntimeFailureAndInvalidReservationStaysIncomplete) {
    EXPECT_TRUE(RuntimeWireContract());
    const auto plan = Plan();
    RuntimeBackend invalid_camera(plan);
    Harness::WorldVisualTemporalTransport invalid_camera_transport(invalid_camera);
    EXPECT_EQ(RunTemporalSweep(
                  plan, Budget(plan), nullptr, kWorldSweepAngles.size(), invalid_camera_transport),
              "sweep_unknown_camera");
    EXPECT_EQ(invalid_camera.subject.renders, 0U);
    EXPECT_EQ(invalid_camera.publish_attempts, 0U);
    for (const auto fault : {RuntimeBackend::Fault::Begin,
                             RuntimeBackend::Fault::Simulation,
                             RuntimeBackend::Fault::WeatherTick,
                             RuntimeBackend::Fault::WeatherRender,
                             RuntimeBackend::Fault::Render,
                             RuntimeBackend::Fault::NotReady,
                             RuntimeBackend::Fault::Capture,
                             RuntimeBackend::Fault::EmptyCapture,
                             RuntimeBackend::Fault::Publish,
                             RuntimeBackend::Fault::EmptyPublish}) {
        RuntimeBackend runtime(plan);
        runtime.fault = fault;
        EXPECT_NE(Harness::RunWorldVisualTemporalSweep(plan, Budget(plan), runtime), "");
        EXPECT_EQ(runtime.subject.publications, 0U);
        if (fault != RuntimeBackend::Fault::Publish &&
            fault != RuntimeBackend::Fault::EmptyPublish) {
            EXPECT_EQ(runtime.publish_attempts, 0U);
        }
        if (fault == RuntimeBackend::Fault::Capture) {
            EXPECT_EQ(runtime.subject.captures.size(), 1U);
        }
    }
    for (const bool bad_plan : {false, true}) {
        auto altered = plan;
        auto budget = Budget(plan);
        if (bad_plan) {
            altered.frames.clear();
        } else {
            --budget.output_bytes;
        }
        RuntimeBackend runtime(plan);
        EXPECT_NE(Harness::RunWorldVisualTemporalSweep(altered, budget, runtime), "");
        EXPECT_EQ(runtime.begins, 0U);
        EXPECT_EQ(runtime.subject.renders, 0U);
        EXPECT_EQ(runtime.publish_attempts, 0U);
    }
}

// Model the app-owned render boundary with the admitted runtime's own sequence.
struct AdmittedRuntimeBackend : RuntimeBackend {
    Harness::RuntimeTemporalAudit& audit;
    AdmittedRuntimeBackend(Harness::RuntimeTemporalAudit& runtime)
        : RuntimeBackend(runtime.plan)
        , audit(runtime) {}
    TemporalSweepOutput capture(const TemporalSequenceStep& step) {
        const auto output = RuntimeBackend::capture(step);
        audit.sequence->acknowledge(actual_tick(), output.success, true, output.bytes);
        return output;
    }
    TemporalSweepOutput publish(const TemporalSequenceStep& step) {
        const auto output = RuntimeBackend::publish(step);
        audit.sequence->acknowledge(actual_tick(), output.success, true, output.bytes);
        return output;
    }
};

TEST(TemporalSweepRuntime, AdmittedSequenceSurvivesAppRenderBoundariesAndStickyFailures) {
    const auto plan = Plan(TemporalAuditSpan::WeatherCycle, 30);
    for (const auto fault : {RuntimeBackend::Fault::None,
                             RuntimeBackend::Fault::WeatherTick,
                             RuntimeBackend::Fault::WeatherRender,
                             RuntimeBackend::Fault::Render,
                             RuntimeBackend::Fault::NotReady,
                             RuntimeBackend::Fault::Capture,
                             RuntimeBackend::Fault::EmptyCapture,
                             RuntimeBackend::Fault::Publish,
                             RuntimeBackend::Fault::EmptyPublish}) {
        SCOPED_TRACE(static_cast<int>(fault));
        Harness::RuntimeTemporalAudit audit{.plan = plan, .budget = Budget(plan)};
        audit.sequence.emplace(plan, audit.budget);
        AdmittedRuntimeBackend backend(audit);
        backend.fault = fault;
        backend.subject.retries = 2;
        Harness::WorldVisualTemporalTransport transport(backend);
        const auto bound = plan.max_render_attempts + plan.frames.size() + 1;
        for (std::uint64_t attempt = 0; attempt < bound; ++attempt) {
            audit.step = Harness::DriveTemporalAuditTicks(
                *audit.sequence,
                [&] { return transport.actual_tick(); },
                [&] {
                    return transport.advance_tick({.action = Action::AdvanceTick,
                                                   .simulation_tick = transport.actual_tick() + 1});
                });
            if (audit.step->action == Action::Refused) {
                break;
            }
            if (!transport.prepare_render(*audit.step)) {
                audit.sequence->refuse();
                break;
            }
            const auto refusal = Harness::RunWorldVisualTemporalSweep(audit, backend, {});
            if (!refusal.empty() || audit.sequence->complete()) {
                break;
            }
            audit.step.reset();
        }
        if (fault == RuntimeBackend::Fault::None) {
            EXPECT_TRUE(audit.sequence->complete());
            EXPECT_EQ(backend.subject.captures.size(), plan.frames.size());
            EXPECT_EQ(backend.subject.publications, 1U);
            EXPECT_EQ(backend.subject.ticks, plan.end_tick - plan.request.start_tick);
        } else {
            ASSERT_FALSE(audit.sequence->refusal().empty());
            EXPECT_FALSE(audit.sequence->complete());
            EXPECT_EQ(backend.subject.publications, 0U);
            const auto renders = backend.subject.renders;
            const auto publications = backend.publish_attempts;
            EXPECT_FALSE(Harness::RunWorldVisualTemporalSweep(audit, backend, {}).empty());
            EXPECT_EQ(backend.subject.renders, renders);
            EXPECT_EQ(backend.publish_attempts, publications);
        }
    }
}

TEST(TemporalSweepRuntime, BothStartupPathsUseFixedSiteAuditAdmission) {
    const auto directory = Luminumbra::TestSupport::CreateUniqueTempDirectory("sweep-runtime-");
    for (const bool environment : {false, true}) {
        ScopedSweepEnvironment override(environment ? "1" : nullptr);
        for (const std::string extra : {"",
                                        "--temporal-start-tick=1",
                                        "--temporal-tick-rate=7",
                                        "--timelapse-frames=0",
                                        "--frame-scan=",
                                        "--scene-config=",
                                        "--render-benchmark=",
                                        "--survey",
                                        "--load-world=existing",
                                        "--runtime-boot-metrics",
                                        "--ui-screenshot=",
                                        "--ui-thumbs",
                                        "--bake-tree-impostor=",
                                        "--render-parity-frame=1",
                                        "--upscale-seam-parity"}) {
            std::vector<std::string> args{"client",
                                          "--temporal-audit-plan=day-night",
                                          "--temporal-start-tick=0",
                                          "--temporal-tick-rate=30",
                                          "--temporal-day-ticks=162000",
                                          "--temporal-dawn-tick=27000",
                                          "--temporal-dusk-tick=135000",
                                          "--scenario=temporal_audit",
                                          "--capture-diagnostics"};
            if (extra.starts_with("--temporal-start")) {
                args[2] = extra;
            } else if (extra.starts_with("--temporal-tick-rate")) {
                args[3] = extra;
            } else if (!extra.empty()) {
                args.push_back(extra);
            }
            const auto save_root = directory / "uncreated";
            if (!extra.empty()) {
                args.push_back("--save-root=" + save_root.string());
            }
            std::vector<char*> argv;
            for (auto& argument : args) {
                argv.push_back(argument.data());
            }
            const auto argc = static_cast<int>(argv.size());
            if (extra.empty()) {
                EXPECT_NO_THROW((void)Harness::ParseRuntimePathOptions(argc, argv.data()));
                const auto config =
                    Harness::ParseRuntimeScenarioConfig(argc, argv.data(), directory);
                ASSERT_TRUE(config.temporal_audit_plan);
                ASSERT_TRUE(config.temporal_audit);
                EXPECT_EQ(config.temporal_audit_plan, config.temporal_audit->plan);
                EXPECT_EQ(config.scenario, "temporal_audit");
                EXPECT_TRUE(config.auto_create_world);
                EXPECT_TRUE(config.auto_enter_world);
            } else {
                EXPECT_THROW((void)Harness::ParseRuntimePathOptions(argc, argv.data()),
                             std::invalid_argument);
                EXPECT_THROW(
                    (void)Harness::ParseRuntimeScenarioConfig(argc, argv.data(), directory),
                    std::invalid_argument);
                EXPECT_FALSE(std::filesystem::exists(save_root));
            }
        }
    }
    for (const bool environment : {false, true}) {
        ScopedSweepEnvironment override(environment ? "1" : nullptr);
        std::vector<std::string> args{"client",
                                      "--temporal-audit-plan=day-night",
                                      "--temporal-start-tick=0",
                                      "--temporal-tick-rate=30",
                                      "--temporal-day-ticks=162000",
                                      "--temporal-dawn-tick=27000",
                                      "--temporal-dusk-tick=135000",
                                      "--capture-diagnostics",
                                      "--save-root=" + (directory / "uncreated").string()};
        if (!environment) {
            args.emplace_back("--scenario=world_visual_sweep");
        }
        std::vector<char*> argv;
        for (auto& argument : args) {
            argv.push_back(argument.data());
        }
        const auto argc = static_cast<int>(argv.size());
        EXPECT_THROW((void)Harness::ParseRuntimePathOptions(argc, argv.data()),
                     std::invalid_argument);
        EXPECT_THROW((void)Harness::ParseRuntimeScenarioConfig(argc, argv.data(), directory),
                     std::invalid_argument);
        EXPECT_FALSE(std::filesystem::exists(directory / "uncreated"));
    }
    const auto plan = Plan(TemporalAuditSpan::WeatherCycle, 30);
    const auto schedule = Harness::WorldVisualSweepWeatherSchedule(plan);
    ASSERT_TRUE(schedule.valid());
    ASSERT_EQ(schedule.phases.size(), 5U);
    EXPECT_EQ(schedule.phases.front().start_tick, plan.request.start_tick);
    EXPECT_EQ(schedule.phases[1].start_tick, plan.request.start_tick + 600);
    EXPECT_EQ(schedule.phases[2].start_tick, plan.request.start_tick + 1500);
    EXPECT_EQ(schedule.phases[3].start_tick, plan.request.start_tick + 2700);
    EXPECT_EQ(schedule.phases.back().start_tick, plan.end_tick);
    EXPECT_EQ(schedule.strike_tick, plan.request.start_tick + plan.request.strike_tick);
    EXPECT_EQ(schedule.phases[2].precipitation, 65535U);
    EXPECT_EQ(schedule.phases.back().precipitation, 0U);
    std::filesystem::remove_all(directory);
}

} // namespace
