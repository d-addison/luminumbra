#include "../support/UniqueTempPath.h"
#include "core/RuntimeScenarioConfig.h"
#include "rendering/TemporalAudit.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {
using namespace Luminumbra::Rendering;
namespace Harness = Luminumbra::Client::ScenarioHarness;
constexpr std::uint64_t clock_limit = std::uint64_t{1} << 62;

TemporalAuditRequest Request(TemporalAuditSpan span = TemporalAuditSpan::DayNight) {
    return {.span = span,
            .start_tick = 0,
            .ticks_per_day = 162000,
            .ticks_per_second = 30,
            .dawn_tick = 27000,
            .dusk_tick = 135000,
            .strike_tick = span == TemporalAuditSpan::WeatherCycle ? 1800U : 0U,
            .night_moon_phase_milli = 1000,
            .tick_limit = kTemporalMaxTicks,
            .frame_limit = kTemporalMaxFrames};
}

bool HasSequenceFrame(const TemporalAuditPlan& plan, std::uint64_t tick, std::uint32_t phase = 0) {
    return std::ranges::any_of(plan.frames, [tick, phase](const auto& frame) {
        return frame.kind == TemporalAuditFrameKind::Sequence && frame.target_tick == tick &&
               frame.sub_tick_60 == phase;
    });
}

template<typename Callback>
auto WithArguments(std::vector<std::string> arguments, const Callback& callback) {
    std::vector<char*> argv;
    for (auto& argument : arguments)
        argv.push_back(argument.data());
    return callback(static_cast<int>(argv.size()), argv.data());
}

auto Parse(std::vector<std::string> arguments) {
    return WithArguments(std::move(arguments), Harness::ParseTemporalAuditPlanOptions);
}

std::vector<std::string> Arguments(const std::string& span = "day-night", bool assigned = true) {
    const std::array<std::pair<std::string, std::string>, 6> pairs = {
        {{"--temporal-audit-plan", span},
         {"--temporal-start-tick", "0"},
         {"--temporal-tick-rate", "30"},
         {"--temporal-day-ticks", "162000"},
         {"--temporal-dawn-tick", "27000"},
         {"--temporal-dusk-tick", "135000"}}};
    std::vector<std::string> arguments{"client"};
    for (const auto& [flag, value] : pairs) {
        if (assigned)
            arguments.push_back(flag + "=" + value);
        else
            arguments.insert(arguments.end(), {flag, value});
    }
    if (span == "weather-cycle") {
        if (assigned)
            arguments.emplace_back("--temporal-strike-tick=1800");
        else
            arguments.insert(arguments.end(), {"--temporal-strike-tick", "1800"});
    }
    return arguments;
}

TEST(TemporalAudit, ThreeSpansAreDeterministicInclusiveAndUseCeilingCadence) {
    for (const auto span : {TemporalAuditSpan::DayNight,
                            TemporalAuditSpan::WeatherCycle,
                            TemporalAuditSpan::MultiDay}) {
        auto request = Request(span);
        request.start_tick = 12345;
        request.ticks_per_day = 162001;
        const auto admitted = BuildTemporalAuditPlan(request);
        ASSERT_TRUE(admitted.plan) << admitted.reason;
        if (!admitted.plan)
            return;
        ASSERT_EQ(admitted.reason, "");
        const auto& plan = *admitted.plan;
        const auto again = BuildTemporalAuditPlan(request);
        ASSERT_TRUE(again.plan);
        if (!again.plan)
            return;
        EXPECT_EQ(plan, *again.plan);
        EXPECT_EQ(plan.request, request);
        const std::uint64_t duration = span == TemporalAuditSpan::WeatherCycle ? 3600U
                                       : span == TemporalAuditSpan::MultiDay   ? 486003U
                                                                               : 162001U;
        const std::uint64_t cadence = span == TemporalAuditSpan::WeatherCycle ? 30U
                                      : span == TemporalAuditSpan::MultiDay   ? 676U
                                                                              : 226U;
        EXPECT_EQ(plan.end_tick, request.start_tick + duration);
        EXPECT_EQ(plan.sparse_interval_ticks, cadence);
        for (std::uint64_t offset = 0; offset <= duration; offset += cadence) {
            EXPECT_TRUE(HasSequenceFrame(plan, request.start_tick + offset));
        }
        EXPECT_TRUE(HasSequenceFrame(plan, plan.end_tick));
        EXPECT_LE(plan.frames.size(), 4096U);
        EXPECT_LE(duration, kTemporalMaxTicks);
        for (const auto& frame : plan.frames) {
            EXPECT_GE(frame.target_tick, request.start_tick);
            EXPECT_LE(frame.target_tick, plan.end_tick);
            EXPECT_LT(frame.sub_tick_60, 60U);
        }
    }
}

TEST(TemporalAudit, ActualMidnightsAndElapsedDayEndpointsAreBothIncluded) {
    auto request = Request(TemporalAuditSpan::MultiDay);
    request.start_tick = 27000;
    const auto admitted = BuildTemporalAuditPlan(request);
    ASSERT_TRUE(admitted.plan);
    if (!admitted.plan)
        return;
    EXPECT_EQ(admitted.plan->boundary_ticks,
              (std::vector<std::uint64_t>{27000, 162000, 189000, 324000, 351000, 486000, 513000}));
    for (const auto tick : admitted.plan->boundary_ticks) {
        EXPECT_TRUE(HasSequenceFrame(*admitted.plan, tick));
    }
    // Starting exactly at dawn includes a complete shifted burst at the final dawn.
    EXPECT_TRUE(HasSequenceFrame(*admitted.plan, 513000));
}

TEST(TemporalAudit, SixtyHzBurstsUseExactRationalSubticksAtEveryRecordedRate) {
    for (const std::uint32_t rate : {1U, 7U, 30U, 59U, 60U, 120U, 1000U}) {
        auto request = Request();
        request.ticks_per_second = rate;
        const auto admitted = BuildTemporalAuditPlan(request);
        ASSERT_TRUE(admitted.plan) << admitted.reason;
        if (!admitted.plan)
            return;
        for (const auto crossing : {request.dawn_tick, request.dusk_tick}) {
            for (std::uint32_t index = 0; index < 120; ++index) {
                const auto numerator = std::uint64_t{index} * rate;
                const auto tick = crossing - rate + numerator / 60;
                const auto phase = static_cast<std::uint32_t>(numerator % 60);
                EXPECT_TRUE(HasSequenceFrame(*admitted.plan, tick, phase)) << rate << ":" << index;
            }
        }
    }
    const auto admitted = BuildTemporalAuditPlan(Request());
    ASSERT_TRUE(admitted.plan);
    if (!admitted.plan)
        return;
    EXPECT_TRUE(HasSequenceFrame(*admitted.plan, 26970, 0));
    EXPECT_TRUE(HasSequenceFrame(*admitted.plan, 26970, 30));
    EXPECT_TRUE(HasSequenceFrame(*admitted.plan, 26971, 0));
}

TEST(TemporalAudit, WeatherBoundariesStrikeAndOverlappingBurstsAreDeduplicated) {
    auto request = Request(TemporalAuditSpan::WeatherCycle);
    request.strike_tick = 1500; // deliberately overlaps the storm boundary burst
    const auto admitted = BuildTemporalAuditPlan(request);
    ASSERT_TRUE(admitted.plan);
    if (!admitted.plan)
        return;
    const auto& plan = *admitted.plan;
    EXPECT_EQ(plan.boundary_ticks, (std::vector<std::uint64_t>{0, 600, 1500, 2700, 3600}));
    for (const auto tick : plan.boundary_ticks) {
        EXPECT_TRUE(HasSequenceFrame(plan, tick));
    }
    EXPECT_TRUE(HasSequenceFrame(plan, request.strike_tick));
    std::uint64_t previous_time = 0;
    bool first = true;
    for (const auto& frame : plan.frames) {
        if (frame.kind != TemporalAuditFrameKind::Sequence)
            continue;
        EXPECT_EQ(frame.control, TemporalAuditControl::Noon);
        const auto time = frame.target_tick * 60 + frame.sub_tick_60;
        if (!first) {
            EXPECT_GT(time, previous_time);
        }
        previous_time = time;
        first = false;
    }
    // A duplicate sparse sample keeps the dense flag, hence preserves history.
    const auto boundary = std::ranges::find_if(plan.frames, [](const auto& frame) {
        return frame.target_tick == 1500 && frame.sub_tick_60 == 0;
    });
    ASSERT_NE(boundary, plan.frames.end());
    EXPECT_TRUE(boundary->dense);
    EXPECT_EQ(std::ranges::count_if(plan.frames,
                                    [](const auto& frame) {
                                        return frame.kind == TemporalAuditFrameKind::Sequence &&
                                               frame.dense;
                                    }),
              600);
}

TEST(TemporalAudit, EndpointBurstsRemainCompleteAtFractionalRatesAndShortestCalendar) {
    for (const std::uint32_t rate : {1U, 7U, 30U, 59U, 60U, 120U, 1000U}) {
        auto request = Request(TemporalAuditSpan::WeatherCycle);
        request.ticks_per_second = rate;
        request.strike_tick = 60ULL * rate;
        const auto admitted = BuildTemporalAuditPlan(request);
        ASSERT_TRUE(admitted.plan);
        if (!admitted.plan)
            return;
        const auto last_numerator = 119ULL * rate;
        const auto extent = last_numerator / 60 + (last_numerator % 60 != 0 ? 1 : 0);
        const std::uint64_t endpoint_start = 120ULL * rate - extent;
        for (const auto start : {std::uint64_t{0}, endpoint_start}) {
            for (std::uint32_t index = 0; index < 120; ++index) {
                const auto numerator = std::uint64_t{index} * rate;
                EXPECT_TRUE(HasSequenceFrame(*admitted.plan,
                                             start + numerator / 60,
                                             static_cast<std::uint32_t>(numerator % 60)));
            }
        }
    }
    auto request = Request();
    request.ticks_per_day = 3;
    request.dawn_tick = 1;
    request.dusk_tick = 2;
    request.ticks_per_second = 1;
    request.tick_limit = 3;
    const auto admitted = BuildTemporalAuditPlan(request);
    ASSERT_TRUE(admitted.plan) << admitted.reason;
    if (!admitted.plan)
        return;
    EXPECT_EQ(admitted.plan->end_tick, 3U);
    EXPECT_EQ(admitted.plan->presentation_steps, 300U);
    request = Request();
    request.ticks_per_second = 7;
    const auto fractional = BuildTemporalAuditPlan(request);
    ASSERT_TRUE(fractional.plan);
    if (!fractional.plan)
        return;
    EXPECT_EQ(fractional.plan->presentation_steps, 1388692U);
}

TEST(TemporalAudit, HoldsAndReadinessHaveExplicitFiniteBudgetsAndFrozenTicks) {
    for (const auto span : {TemporalAuditSpan::DayNight, TemporalAuditSpan::WeatherCycle}) {
        const auto admitted = BuildTemporalAuditPlan(Request(span));
        ASSERT_TRUE(admitted.plan);
        if (!admitted.plan)
            return;
        const auto& plan = *admitted.plan;
        const bool weather = span == TemporalAuditSpan::WeatherCycle;
        EXPECT_EQ(plan.settling.warmup_frames, 90U);
        EXPECT_EQ(plan.settling.initial_readiness_retries, 300U);
        EXPECT_EQ(plan.settling.sample_settle_frames, 3U);
        EXPECT_EQ(plan.settling.sample_readiness_retries, 60U);
        std::array<std::uint32_t, 3> counts{};
        for (const auto& frame : plan.frames) {
            if (frame.kind == TemporalAuditFrameKind::Sequence)
                continue;
            const auto kind = static_cast<std::size_t>(frame.kind);
            EXPECT_EQ(frame.target_tick, plan.end_tick);
            EXPECT_EQ(frame.sub_tick_60, 0U);
            EXPECT_EQ(frame.hold_frame, counts[kind]++);
            EXPECT_TRUE(frame.dense);
            if (frame.kind == TemporalAuditFrameKind::NightPresenceHold) {
                EXPECT_EQ(frame.control, TemporalAuditControl::Night);
            }
        }
        EXPECT_EQ(counts[1], 120U);
        EXPECT_EQ(counts[2], weather ? 120U : 0U);
        EXPECT_EQ(plan.presentation_steps, weather ? 7440U : 324120U);
        EXPECT_EQ(plan.max_render_attempts,
                  plan.presentation_steps + 390 + plan.frames.size() * 64);
        EXPECT_EQ(plan.request.night_moon_phase_milli, 1000U);
    }
}

TEST(TemporalAudit, AdvancementIsIndependentOfRenderPacingAndRefusesClockFaults) {
    auto request = Request();
    request.start_tick = 100;
    const auto admitted = BuildTemporalAuditPlan(request);
    ASSERT_TRUE(admitted.plan);
    if (!admitted.plan)
        return;
    const auto& plan = *admitted.plan;
    auto current_tick = request.start_tick;
    std::uint64_t total = 0;
    for (std::size_t index = 0; index < plan.frames.size(); ++index) {
        const auto ticks = TemporalAuditTicksToFrame(plan, index, current_tick);
        ASSERT_TRUE(ticks);
        if (!ticks)
            return;
        // Arbitrarily many render/readiness queries consume no simulation time.
        for (int render = 0; render < 5; ++render) {
            EXPECT_EQ(TemporalAuditTicksToFrame(plan, index, current_tick), ticks);
        }
        current_tick += *ticks;
        total += *ticks;
        EXPECT_EQ(current_tick, plan.frames[index].target_tick);
        EXPECT_EQ(TemporalAuditTicksToFrame(plan, index, current_tick), 0U);
        EXPECT_FALSE(TemporalAuditTicksToFrame(plan, index, current_tick + 1));
    }
    EXPECT_EQ(total, request.ticks_per_day);
    EXPECT_FALSE(TemporalAuditTicksToFrame(plan, plan.frames.size(), current_tick));
    EXPECT_FALSE(TemporalAuditTicksToFrame(plan, 0, request.start_tick - 1));
    EXPECT_FALSE(TemporalAuditTicksToFrame(TemporalAuditPlan{}, 0, 0));
}

TEST(TemporalAudit, EveryInvalidOrUnboundedRequestRefusesWithReasonWithoutChangingInput) {
    using Case = std::pair<TemporalAuditRequest, std::string>;
    std::vector<Case> cases;
    auto input = Request();
    input.span = static_cast<TemporalAuditSpan>(99);
    cases.emplace_back(input, "unknown temporal span");
    for (const auto value : {0ULL, 10000001ULL, std::numeric_limits<unsigned long long>::max()}) {
        input = Request();
        input.tick_limit = value;
        cases.emplace_back(input, "tick limit must be 1..10000000");
        input = Request();
        input.ticks_per_day = value;
        cases.emplace_back(input, "ticks per day must be 1..10000000");
    }
    for (const std::size_t value :
         {std::size_t{0}, std::size_t{4097}, std::numeric_limits<std::size_t>::max()}) {
        input = Request();
        input.frame_limit = value;
        cases.emplace_back(input, "frame limit must be 1..4096");
    }
    for (const std::uint32_t value : {0U, 1001U, std::numeric_limits<std::uint32_t>::max()}) {
        input = Request();
        input.ticks_per_second = value;
        cases.emplace_back(input, "tick rate must be 1..1000 Hz");
    }
    for (const auto value : {clock_limit, std::numeric_limits<std::uint64_t>::max()}) {
        input = Request();
        input.start_tick = value;
        cases.emplace_back(input, "start tick exceeds simulation clock range");
    }
    for (const auto& crossing : {std::pair{0U, 135000U},
                                 std::pair{135000U, 135000U},
                                 std::pair{140000U, 135000U},
                                 std::pair{27000U, 162000U},
                                 std::pair{27000U, 162001U}}) {
        input = Request();
        input.dawn_tick = crossing.first;
        input.dusk_tick = crossing.second;
        cases.emplace_back(input, "calendar crossings require 0 < dawn < dusk < ticks per day");
    }
    input = Request();
    input.night_moon_phase_milli = 1001;
    cases.emplace_back(input, "moon phase must be 0..1000");
    for (const auto value : {0U, 1499U, 2700U, 2701U}) {
        input = Request(TemporalAuditSpan::WeatherCycle);
        input.strike_tick = value;
        cases.emplace_back(input, "strike tick must be in storm interval [50T,90T)");
    }
    for (const auto span : {TemporalAuditSpan::DayNight, TemporalAuditSpan::MultiDay}) {
        input = Request(span);
        input.strike_tick = 1;
        cases.emplace_back(input, "strike tick requires weather-cycle span");
    }
    input = Request();
    input.tick_limit = input.ticks_per_day - 1;
    cases.emplace_back(input, "span exceeds tick budget");
    input = Request(TemporalAuditSpan::MultiDay);
    input.ticks_per_day = kTemporalMaxTicks;
    cases.emplace_back(input, "span exceeds tick budget");
    input = Request();
    input.start_tick = clock_limit - input.ticks_per_day;
    cases.emplace_back(input, "end tick exceeds simulation clock range");
    input = Request();
    input.ticks_per_day = 3;
    input.dawn_tick = 1;
    input.dusk_tick = 2;
    cases.emplace_back(input, "span cannot contain a complete 120-frame burst");
    input = Request();
    input.frame_limit = 1;
    cases.emplace_back(input, "span exceeds frame budget");
    for (const auto& [request, reason] : cases) {
        const auto before = request;
        const auto admitted = BuildTemporalAuditPlan(request);
        EXPECT_FALSE(admitted.plan) << reason;
        EXPECT_EQ(admitted.reason, reason);
        EXPECT_EQ(request, before);
    }
}

TEST(TemporalAudit, ExactTickFrameClockAndControlLimitsAreAccepted) {
    auto request = Request();
    request.ticks_per_day = kTemporalMaxTicks;
    request.start_tick = clock_limit - 1 - kTemporalMaxTicks;
    request.dusk_tick = kTemporalMaxTicks - 1;
    request.night_moon_phase_milli = 0;
    const auto admitted = BuildTemporalAuditPlan(request);
    ASSERT_TRUE(admitted.plan) << admitted.reason;
    if (!admitted.plan)
        return;
    EXPECT_EQ(admitted.plan->end_tick, clock_limit - 1);
    request.frame_limit = admitted.plan->frames.size();
    request.tick_limit = kTemporalMaxTicks;
    EXPECT_TRUE(BuildTemporalAuditPlan(request).plan);
    --request.frame_limit;
    EXPECT_EQ(BuildTemporalAuditPlan(request).reason, "span exceeds frame budget");
    for (const auto strike : {1500U, 2699U}) {
        request = Request(TemporalAuditSpan::WeatherCycle);
        request.strike_tick = strike;
        request.tick_limit = 3600;
        EXPECT_TRUE(BuildTemporalAuditPlan(request).plan);
        --request.tick_limit;
        EXPECT_EQ(BuildTemporalAuditPlan(request).reason, "span exceeds tick budget");
    }
}

TEST(TemporalAudit, PlanParserAcceptsAllSpansBothSpellingsAndOptionalBounds) {
    EXPECT_FALSE(Parse({"client"}));
    for (const bool assigned : {false, true}) {
        for (const std::string span : {"day-night", "weather-cycle", "multi-day"}) {
            const auto arguments = Arguments(span, assigned);
            const auto parsed = Parse(arguments);
            ASSERT_TRUE(parsed);
            if (!parsed)
                return;
            const auto request = Request(span == "day-night"       ? TemporalAuditSpan::DayNight
                                         : span == "weather-cycle" ? TemporalAuditSpan::WeatherCycle
                                                                   : TemporalAuditSpan::MultiDay);
            const auto built = BuildTemporalAuditPlan(request);
            ASSERT_TRUE(built.plan);
            if (!built.plan)
                return;
            EXPECT_EQ(*parsed, *built.plan);
        }
    }
    auto arguments = Arguments("weather-cycle");
    arguments.insert(arguments.end(),
                     {"--temporal-tick-limit=3600",
                      "--temporal-frame-limit=4096",
                      "--temporal-moon-phase-milli=0"});
    const auto parsed = Parse(arguments);
    ASSERT_TRUE(parsed);
    if (!parsed)
        return;
    EXPECT_EQ(parsed->request.tick_limit, 3600U);
    EXPECT_EQ(parsed->request.frame_limit, 4096U);
    EXPECT_EQ(parsed->request.night_moon_phase_milli, 0U);
}

TEST(TemporalAudit, PlanParserRefusesMissingAndDuplicateOptions) {
    const auto valid = Arguments();
    for (std::size_t index = 1; index < valid.size(); ++index) {
        auto arguments = valid;
        arguments.erase(arguments.begin() + static_cast<std::ptrdiff_t>(index));
        EXPECT_THROW(Parse(arguments), std::invalid_argument);
    }
    for (const std::string flag : {"--temporal-audit-plan",
                                   "--temporal-start-tick",
                                   "--temporal-tick-rate",
                                   "--temporal-day-ticks",
                                   "--temporal-dawn-tick",
                                   "--temporal-dusk-tick",
                                   "--temporal-strike-tick",
                                   "--temporal-frame-limit",
                                   "--temporal-tick-limit",
                                   "--temporal-moon-phase-milli"}) {
        for (const bool assigned : {false, true}) {
            auto missing = valid;
            const auto existing = std::ranges::find_if(missing, [&flag](const auto& argument) {
                return argument.starts_with(flag + "=");
            });
            if (existing != missing.end())
                missing.erase(existing);
            missing.push_back(flag + (assigned ? "=" : ""));
            EXPECT_THROW(Parse(missing), std::invalid_argument);
            auto duplicate = Arguments("weather-cycle", assigned);
            duplicate.insert(duplicate.end(), {flag + "=1", flag, "2"});
            EXPECT_THROW(Parse(duplicate), std::invalid_argument);
        }
    }
}

TEST(TemporalAudit, PlanParserRefusesPartialSignedAndOverflowNumbers) {
    const auto valid = Arguments();
    for (const std::string flag : {"--temporal-start-tick",
                                   "--temporal-tick-rate",
                                   "--temporal-day-ticks",
                                   "--temporal-dawn-tick",
                                   "--temporal-dusk-tick",
                                   "--temporal-strike-tick",
                                   "--temporal-frame-limit",
                                   "--temporal-tick-limit",
                                   "--temporal-moon-phase-milli"}) {
        for (const std::string value : {"-1",
                                        "+1",
                                        " 1",
                                        "1 ",
                                        "1junk",
                                        "1e38",
                                        "nan",
                                        "inf",
                                        "18446744073709551616",
                                        "99999999999999999999999999999999"}) {
            // Replace required options rather than testing a duplicate refusal.
            auto arguments = valid;
            const auto found = std::ranges::find_if(arguments, [&flag](const auto& argument) {
                return argument.starts_with(flag + "=");
            });
            if (found != arguments.end())
                arguments.erase(found);
            for (const bool assigned : {false, true}) {
                auto malformed = arguments;
                if (assigned)
                    malformed.push_back(flag + "=" + value);
                else
                    malformed.insert(malformed.end(), {flag, value});
                EXPECT_THROW(Parse(malformed), std::invalid_argument) << flag << ":" << value;
            }
        }
    }
}

TEST(TemporalAudit, PlanParserRefusesUnknownSpansAndInvalidCrossFieldBounds) {
    const auto valid = Arguments();
    for (const std::string value : {"", "day", "weather", "multi-day:3"}) {
        auto arguments = valid;
        arguments[1] = "--temporal-audit-plan=" + value;
        EXPECT_THROW(Parse(arguments), std::invalid_argument);
    }
    auto unknown = valid;
    unknown.emplace_back("--temporal-unknown=1");
    EXPECT_THROW(Parse(unknown), std::invalid_argument);
    for (const auto& replacement : {std::pair{2U, "--temporal-start-tick=4611686018427387904"},
                                    std::pair{3U, "--temporal-tick-rate=0"},
                                    std::pair{3U, "--temporal-tick-rate=4294967296"},
                                    std::pair{4U, "--temporal-day-ticks=0"},
                                    std::pair{5U, "--temporal-dawn-tick=0"},
                                    std::pair{6U, "--temporal-dusk-tick=1"}}) {
        auto arguments = valid;
        arguments[replacement.first] = replacement.second;
        EXPECT_THROW(Parse(arguments), std::invalid_argument);
    }
    for (const std::string value : {"--temporal-moon-phase-milli=4294967296",
                                    "--temporal-frame-limit=18446744073709551615",
                                    "--temporal-tick-limit=0",
                                    "--temporal-frame-limit=0",
                                    "--temporal-frame-limit=1",
                                    "--temporal-strike-tick=1"}) {
        auto arguments = valid;
        arguments.push_back(value);
        EXPECT_THROW(Parse(arguments), std::invalid_argument);
    }
    auto missing_strike = Arguments("weather-cycle");
    missing_strike.pop_back();
    EXPECT_THROW(Parse(missing_strike), std::invalid_argument);
}

TEST(TemporalAudit, BothRuntimeEntryPathsRefuseBeforeCreatingOutputsWithExplicitReason) {
    const auto directory =
        Luminumbra::TestSupport::CreateUniqueTempDirectory("temporal-admission-");
    const auto save_root = directory / "absent";
    for (const bool paths : {false, true}) {
        for (const bool first : {false, true}) {
            for (const auto& temporal :
                 {Arguments(),
                  Arguments("weather-cycle"),
                  Arguments("multi-day"),
                  std::vector<std::string>{"client", "--scenario=temporal_audit"},
                  std::vector<std::string>{"client", "--temporal-audit-plan=bad"}}) {
                auto arguments = temporal;
                const bool invalid = temporal.back() == "--temporal-audit-plan=bad";
                if (first)
                    arguments.insert(arguments.begin() + 1, {"--save-root", save_root.string()});
                else
                    arguments.insert(arguments.end(), {"--save-root", save_root.string()});
                try {
                    WithArguments(arguments, [paths](int argc, char* argv[]) {
                        if (paths)
                            static_cast<void>(Harness::ParseRuntimePathOptions(argc, argv));
                        else
                            static_cast<void>(Harness::ParseRuntimeScenarioConfig(argc, argv, {}));
                    });
                    FAIL() << "runtime execution must refuse";
                } catch (const std::invalid_argument& error) {
                    EXPECT_EQ(std::string(error.what()),
                              invalid
                                  ? "unknown temporal span"
                                  : "temporal audit execution unsupported: plan admission only");
                }
                EXPECT_FALSE(std::filesystem::exists(save_root));
            }
        }
    }
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    EXPECT_FALSE(error);
}
} // namespace
