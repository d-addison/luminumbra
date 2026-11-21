#include "rendering/TemporalAudit.h"

#include "luminumbra_common/ai/CreatureBrainSystem.h"
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>

namespace {
using namespace Luminumbra::Rendering;
using Action = TemporalSequenceAction;
namespace Comp = Luminumbra::Components;

TemporalAuditPlan Plan(TemporalAuditSpan span = TemporalAuditSpan::DayNight,
                       std::uint32_t rate = 30) {
    const auto admission = BuildTemporalAuditPlan(
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
    EXPECT_TRUE(admission.plan) << admission.reason;
    return admission.plan ? *admission.plan : TemporalAuditPlan{};
}

TemporalSequenceBudget Budget(const TemporalAuditPlan& plan) {
    return {.frame_bytes = 1024,
            .manifest_bytes = 4096,
            .output_bytes = static_cast<std::uint64_t>(plan.frames.size()) * 1024 + 4096};
}

void Accept(TemporalAuditSequence& sequence,
            const TemporalSequenceStep& command,
            bool ready = true) {
    const bool output = command.action == Action::Capture || command.action == Action::Publish;
    sequence.acknowledge(command.simulation_tick, true, ready, output ? command.byte_limit : 0);
}

void Reach(TemporalAuditSequence& sequence, Action action, std::uint64_t& tick) {
    // All fixtures use short calendars: < 20000 steps to any requested state.
    for (unsigned attempt = 0; attempt < 20000; ++attempt) {
        const auto command = sequence.next(tick);
        if (command.action == action) {
            return;
        }
        ASSERT_NE(command.action, Action::Refused) << sequence.refusal();
        ASSERT_NE(command.action, Action::Complete);
        tick = command.simulation_tick;
        Accept(sequence, command);
    }
    FAIL() << "sequence did not reach requested state";
}

TEST(TemporalAuditSequence, ExactEndpointsBurstsAndHoldsWithoutExtraFrameDeltaTicks) {
    for (const auto span : {TemporalAuditSpan::DayNight,
                            TemporalAuditSpan::WeatherCycle,
                            TemporalAuditSpan::MultiDay}) {
        for (const std::uint32_t rate : {1U, 7U, 30U, 120U}) {
            SCOPED_TRACE(rate);
            SCOPED_TRACE(static_cast<int>(span));
            const auto plan = Plan(span, rate);
            TemporalAuditSequence sequence(plan, Budget(plan));
            auto tick = plan.request.start_tick;
            std::size_t captures = 0;
            std::uint64_t ticks = 0;
            std::uint64_t presentations = 0;
            bool published = false;
            const auto bound = plan.end_tick - plan.request.start_tick + plan.max_render_attempts +
                               2 * plan.frames.size() + 1;
            for (std::uint64_t attempt = 0; attempt < bound && !sequence.complete(); ++attempt) {
                const auto command = sequence.next(tick);
                ASSERT_NE(command.action, Action::Refused) << sequence.refusal();
                // Repeated render-loop queries neither consume a step nor tick.
                EXPECT_EQ(sequence.next(tick), command);
                EXPECT_LE(command.presentation_delta_seconds, 1.0 / 60.0);
                EXPECT_GE(command.presentation_delta_seconds, 0.0);
                if (command.action == Action::AdvanceTick) {
                    EXPECT_EQ(command.simulation_tick, tick + 1);
                    ++ticks;
                } else {
                    EXPECT_EQ(command.simulation_tick, tick);
                }
                if (command.action == Action::Present) {
                    ++presentations;
                }
                if (command.action == Action::Capture) {
                    ASSERT_LT(captures, plan.frames.size());
                    const auto& frame = plan.frames[captures];
                    EXPECT_EQ(command.frame_index, captures);
                    EXPECT_EQ(command.simulation_tick, frame.target_tick);
                    EXPECT_EQ(command.sub_tick_60, frame.sub_tick_60);
                    EXPECT_EQ(command.presentation_delta_seconds, 0);
                    EXPECT_FALSE(published);
                    ++captures;
                }
                if (command.action == Action::Publish) {
                    EXPECT_EQ(captures, plan.frames.size());
                    EXPECT_FALSE(sequence.complete());
                    published = true;
                }
                tick = command.simulation_tick;
                Accept(sequence, command);
            }
            EXPECT_TRUE(sequence.complete()) << sequence.refusal();
            EXPECT_TRUE(published);
            EXPECT_EQ(captures, plan.frames.size());
            EXPECT_EQ(sequence.captured_frames(), captures);
            EXPECT_EQ(ticks, plan.end_tick - plan.request.start_tick);
            EXPECT_EQ(tick, plan.end_tick);
            EXPECT_GE(presentations, plan.presentation_steps - 2);
            EXPECT_EQ(sequence.next(tick).action, Action::Complete);
        }
    }
}

TEST(TemporalAuditSequence, WarmupAndReadinessAreFrozenWithExactRetryLimits) {
    const auto plan = Plan();
    const auto tick = plan.request.start_tick;
    for (const bool initial : {true, false}) {
        TemporalAuditSequence sequence(plan, Budget(plan));
        for (std::uint32_t index = 0; index < plan.settling.warmup_frames; ++index) {
            const auto command = sequence.next(tick);
            ASSERT_EQ(command.action, Action::Warmup);
            EXPECT_EQ(command.simulation_tick, tick);
            EXPECT_DOUBLE_EQ(command.presentation_delta_seconds, 1.0 / 60.0);
            Accept(sequence, command);
        }
        auto command = sequence.next(tick);
        ASSERT_EQ(command.action, Action::InitialReadiness);
        if (!initial) {
            Accept(sequence, command);
            auto unchanged_tick = tick;
            Reach(sequence, Action::Readiness, unchanged_tick);
            EXPECT_EQ(unchanged_tick, tick);
        }
        const auto limit = initial ? plan.settling.initial_readiness_retries
                                   : plan.settling.sample_readiness_retries;
        for (std::uint32_t index = 0; index < limit; ++index) {
            command = sequence.next(tick);
            ASSERT_EQ(command.action, initial ? Action::InitialReadiness : Action::Readiness);
            EXPECT_EQ(command.simulation_tick, tick);
            EXPECT_EQ(command.sub_tick_60, 0U);
            EXPECT_EQ(command.presentation_delta_seconds, 0);
            Accept(sequence, command, false);
        }
        EXPECT_EQ(sequence.next(tick).action, Action::Refused);
        EXPECT_EQ(sequence.refusal(), "sequence readiness exhausted");
        EXPECT_EQ(sequence.captured_frames(), 0U);
        EXPECT_FALSE(sequence.complete());
    }
}

TEST(TemporalAuditSequence, LastReadinessRetrySucceedsAndSparseSettlingIsExactlyThreeFrames) {
    const auto plan = Plan();
    TemporalAuditSequence sequence(plan, Budget(plan));
    auto tick = plan.request.start_tick;
    Reach(sequence, Action::InitialReadiness, tick);
    for (std::uint32_t index = 1; index < plan.settling.initial_readiness_retries; ++index) {
        Accept(sequence, sequence.next(tick), false);
    }
    Accept(sequence, sequence.next(tick));
    Reach(sequence, Action::Readiness, tick);
    for (std::uint32_t index = 1; index < plan.settling.sample_readiness_retries; ++index) {
        Accept(sequence, sequence.next(tick), false);
    }
    Accept(sequence, sequence.next(tick));
    EXPECT_EQ(sequence.next(tick).action, Action::Capture);
    unsigned settles = 0;
    std::size_t sparse_frames = 0;
    std::array<unsigned, 4096> per_frame{};
    for (unsigned attempt = 0; attempt < 20000 && !sequence.complete(); ++attempt) {
        const auto command = sequence.next(tick);
        ASSERT_NE(command.action, Action::Refused) << sequence.refusal();
        if (command.action == Action::Settle) {
            EXPECT_EQ(command.presentation_delta_seconds, 0);
            ++settles;
            ++per_frame[command.frame_index];
        }
        tick = command.simulation_tick;
        Accept(sequence, command);
    }
    ASSERT_TRUE(sequence.complete());
    for (std::size_t index = 0; index < plan.frames.size(); ++index) {
        const auto expected = plan.frames[index].dense ? 0U : 3U;
        EXPECT_EQ(per_frame[index], expected);
        sparse_frames += plan.frames[index].dense ? 0U : 1U;
    }
    EXPECT_GT(sparse_frames, 0U);
    EXPECT_EQ(settles, sparse_frames * 3);
}

TEST(TemporalAuditSequence, PhaseAndLightingControlsFollowRationalTicks) {
    for (const auto span : {TemporalAuditSpan::MultiDay, TemporalAuditSpan::WeatherCycle}) {
        const auto plan = Plan(span, 7);
        TemporalAuditSequence sequence(plan, Budget(plan));
        auto tick = plan.request.start_tick;
        for (unsigned attempt = 0; attempt < 20000 && !sequence.complete(); ++attempt) {
            const auto command = sequence.next(tick);
            ASSERT_NE(command.action, Action::Refused) << sequence.refusal();
            const double actual_phase =
                (static_cast<double>(command.simulation_tick % plan.request.ticks_per_day) +
                 static_cast<double>(command.sub_tick_60) / 60.0) /
                static_cast<double>(plan.request.ticks_per_day);
            EXPECT_DOUBLE_EQ(command.day_phase, actual_phase);
            if (command.action == Action::Capture) {
                const auto& frame = plan.frames[command.frame_index];
                const double expected =
                    (static_cast<double>(frame.target_tick % plan.request.ticks_per_day) +
                     static_cast<double>(frame.sub_tick_60) / 60.0) /
                    static_cast<double>(plan.request.ticks_per_day);
                EXPECT_DOUBLE_EQ(command.day_phase, expected);
                EXPECT_EQ(command.control, frame.control);
            }
            tick = command.simulation_tick;
            Accept(sequence, command);
        }
        EXPECT_TRUE(sequence.complete());
    }
}

TEST(TemporalAuditSequence, BudgetAndAlteredPlansRefuseBeforeAnyExecutionOrCounters) {
    const auto plan = Plan();
    auto too_small = Budget(plan);
    --too_small.output_bytes;
    for (const auto budget :
         {too_small,
          TemporalSequenceBudget{},
          TemporalSequenceBudget{.frame_bytes = 1, .manifest_bytes = 0, .output_bytes = 1},
          TemporalSequenceBudget{.frame_bytes = 0, .manifest_bytes = 1, .output_bytes = 1},
          TemporalSequenceBudget{.frame_bytes = 1, .manifest_bytes = 1, .output_bytes = 0},
          TemporalSequenceBudget{
              .frame_bytes = 1, .manifest_bytes = 1, .output_bytes = kTemporalMaxOutputBytes + 1},
          TemporalSequenceBudget{.frame_bytes = std::numeric_limits<std::uint64_t>::max(),
                                 .manifest_bytes = 1,
                                 .output_bytes = kTemporalMaxOutputBytes},
          TemporalSequenceBudget{.frame_bytes = 1,
                                 .manifest_bytes = std::numeric_limits<std::uint64_t>::max(),
                                 .output_bytes = 1}}) {
        TemporalAuditSequence sequence(plan, budget);
        EXPECT_EQ(sequence.next(plan.request.start_tick).action, Action::Refused);
        EXPECT_EQ(sequence.captured_frames(), 0U);
        EXPECT_FALSE(sequence.complete());
    }
    TemporalAuditSequence hard_boundary(
        plan, {.frame_bytes = 1, .manifest_bytes = 1, .output_bytes = kTemporalMaxOutputBytes});
    EXPECT_EQ(hard_boundary.next(plan.request.start_tick).action, Action::Warmup);
    for (unsigned mutation = 0; mutation < 6; ++mutation) {
        auto altered = plan;
        switch (mutation) {
            case 0:
                altered.frames.clear();
                break;
            case 1:
                ++altered.frames.back().target_tick;
                break;
            case 2:
                ++altered.settling.initial_readiness_retries;
                break;
            case 3:
                ++altered.max_render_attempts;
                break;
            case 4:
                altered.request.frame_limit = 1;
                break;
            case 5:
                altered.frames.front().sub_tick_60 = 60;
                break;
        }
        TemporalAuditSequence sequence(altered, Budget(plan));
        EXPECT_EQ(sequence.next(plan.request.start_tick).action, Action::Refused);
        EXPECT_EQ(sequence.captured_frames(), 0U);
    }
}

TEST(TemporalAuditSequence, ClockFaultsDuplicateAcknowledgementsAndTransportFailuresAreSticky) {
    const auto plan = Plan();
    for (const auto bad_tick : {plan.request.start_tick - 1, plan.request.start_tick + 1}) {
        TemporalAuditSequence sequence(plan, Budget(plan));
        EXPECT_EQ(sequence.next(bad_tick).action, Action::Refused);
        EXPECT_EQ(sequence.next(plan.request.start_tick).action, Action::Refused);
    }
    for (const auto action : {Action::Warmup,
                              Action::InitialReadiness,
                              Action::AdvanceTick,
                              Action::Present,
                              Action::Settle,
                              Action::Readiness,
                              Action::Capture,
                              Action::Publish}) {
        for (unsigned fault = 0; fault < 3; ++fault) {
            TemporalAuditSequence sequence(plan, Budget(plan));
            auto tick = plan.request.start_tick;
            Reach(sequence, action, tick);
            const auto command = sequence.next(tick);
            const auto captured = sequence.captured_frames();
            if (fault == 0) {
                sequence.acknowledge(command.simulation_tick, false);
            } else if (fault == 1) {
                sequence.acknowledge(command.simulation_tick + 1, true);
            } else {
                sequence.acknowledge(command.simulation_tick, true, true, command.byte_limit + 1);
            }
            EXPECT_EQ(sequence.next(tick).action, Action::Refused);
            EXPECT_EQ(sequence.captured_frames(), captured);
            EXPECT_FALSE(sequence.complete());
            sequence.acknowledge(tick, true);
            EXPECT_EQ(sequence.next(tick).action, Action::Refused);
        }
    }
    TemporalAuditSequence duplicate(plan, Budget(plan));
    auto command = duplicate.next(plan.request.start_tick);
    Accept(duplicate, command);
    Accept(duplicate, command);
    EXPECT_EQ(duplicate.next(plan.request.start_tick).action, Action::Refused);
    TemporalAuditSequence stalled(plan, Budget(plan));
    auto tick = plan.request.start_tick;
    Reach(stalled, Action::AdvanceTick, tick);
    stalled.acknowledge(tick, true);
    EXPECT_EQ(stalled.next(tick).action, Action::Refused);
}

TEST(TemporalAuditSequence, EmptyCaptureAndFailedPublicationNeverCompletePartialSequence) {
    const auto plan = Plan();
    for (const auto action : {Action::Capture, Action::Publish}) {
        TemporalAuditSequence sequence(plan, Budget(plan));
        auto tick = plan.request.start_tick;
        Reach(sequence, action, tick);
        sequence.acknowledge(tick, true, true, 0);
        EXPECT_EQ(sequence.next(tick).action, Action::Refused);
        EXPECT_FALSE(sequence.complete());
    }
    TemporalAuditSequence cancelled(plan, Budget(plan));
    auto tick = plan.request.start_tick;
    Reach(cancelled, Action::Capture, tick);
    Accept(cancelled, cancelled.next(tick));
    ASSERT_EQ(cancelled.captured_frames(), 1U);
    cancelled.refuse();
    EXPECT_FALSE(cancelled.complete());
    EXPECT_EQ(cancelled.next(tick).action, Action::Refused);
}

TEST(TemporalAuditSequence, CreatureTicksStayLiveBetweenCapturesAndMatchOrdinaryTicking) {
    const auto plan = Plan();
    entt::registry sequence_registry;
    entt::registry ordinary_registry;
    const auto spawn = [](entt::registry& registry) {
        const auto entity = registry.create();
        registry.emplace<Comp::TransformComponent>(entity);
        registry.emplace<Comp::CreatureComponent>(entity).hunger = 0.5f;
        return entity;
    };
    const auto subject = spawn(sequence_registry);
    const auto reference = spawn(ordinary_registry);
    TemporalAuditSequence sequence(plan, Budget(plan));
    auto tick = plan.request.start_tick;
    std::uint64_t creature_ticks = 0;
    unsigned moving_samples = 0;
    float previous_energy = 1.0f;
    for (unsigned attempt = 0; attempt < 20000 && !sequence.complete(); ++attempt) {
        const auto command = sequence.next(tick);
        ASSERT_NE(command.action, Action::Refused) << sequence.refusal();
        if (command.action == Action::AdvanceTick) {
            const auto stats =
                luminumbra::ai::RunCreatureBrainSystemOnTick(sequence_registry, 1.0f / 30.0f);
            EXPECT_EQ(stats.updated, 1);
            ++creature_ticks;
        }
        if (command.action == Action::Capture) {
            const auto energy = sequence_registry.get<Comp::CreatureComponent>(subject).energy;
            if (energy != previous_energy) {
                ++moving_samples;
            }
            previous_energy = energy;
        }
        tick = command.simulation_tick;
        Accept(sequence, command);
    }
    ASSERT_TRUE(sequence.complete());
    EXPECT_EQ(creature_ticks, plan.end_tick - plan.request.start_tick);
    EXPECT_GT(moving_samples, 1U);
    for (auto index = plan.request.start_tick; index < plan.end_tick; ++index) {
        luminumbra::ai::RunCreatureBrainSystemOnTick(ordinary_registry, 1.0f / 30.0f);
    }
    const auto& actual = sequence_registry.get<Comp::CreatureComponent>(subject);
    const auto& expected = ordinary_registry.get<Comp::CreatureComponent>(reference);
    EXPECT_EQ(actual.energy, expected.energy);
    EXPECT_EQ(actual.hunger, expected.hunger);
    EXPECT_EQ(actual.stamina, expected.stamina);
    EXPECT_EQ(actual.last_action, expected.last_action);
    EXPECT_EQ(sequence_registry.get<Comp::TransformComponent>(subject).position,
              ordinary_registry.get<Comp::TransformComponent>(reference).position);
}
} // namespace
