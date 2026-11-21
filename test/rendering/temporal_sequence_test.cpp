#include "rendering/TemporalAudit.h"

#include "../support/UniqueTempPath.h"
#include "luminumbra_common/ai/CreatureBrainSystem.h"
#include "luminumbra_common/authoring/PrefabDigest.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

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

struct CompletionDirectory {
    std::filesystem::path path = Luminumbra::TestSupport::CreateUniqueTempDirectory("temporal-");
    ~CompletionDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

bool WriteBytes(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    return static_cast<bool>(output);
}

TemporalSequenceBudget FileBudget(const TemporalAuditPlan& plan) {
    return {.frame_bytes = 1024,
            .manifest_bytes = 16U * 1024U * 1024U,
            .output_bytes =
                static_cast<std::uint64_t>(plan.frames.size()) * 1024 + 16U * 1024U * 1024U};
}

void ReachFilePublication(TemporalAuditSequence& sequence,
                          const TemporalAuditPlan& plan,
                          const std::filesystem::path& directory) {
    auto tick = plan.request.start_tick;
    std::uint64_t advanced = 0;
    for (unsigned attempt = 0; attempt < 20000; ++attempt) {
        const auto command = sequence.next(tick);
        ASSERT_NE(command.action, Action::Refused) << sequence.refusal();
        if (command.action == Action::Publish) {
            EXPECT_EQ(advanced, plan.end_tick - plan.request.start_tick);
            EXPECT_EQ(tick, plan.end_tick);
            return;
        }
        if (command.action == Action::AdvanceTick) {
            ASSERT_EQ(command.simulation_tick, tick + 1);
            ++advanced;
        } else {
            ASSERT_EQ(command.simulation_tick, tick);
        }
        tick = command.simulation_tick;
        if (command.action == Action::Capture) {
            ASSERT_LT(command.frame_index, plan.frames.size());
            EXPECT_EQ(tick, plan.frames[command.frame_index].target_tick);
            const auto name = "frame-" + std::to_string(command.frame_index) + ".ppm";
            ASSERT_TRUE(WriteBytes(directory / name, "abc"));
            std::vector<std::string> names{name};
            if (command.frame_index == 0) {
                names.push_back("frame-0.ppm.depth.pfm");
                ASSERT_TRUE(WriteBytes(directory / names.back(), std::string_view("\0\r\n", 3)));
            }
            ASSERT_TRUE(sequence.acknowledgeCaptureFiles(tick, directory, names));
            EXPECT_FALSE(sequence.complete());
        } else {
            Accept(sequence, command);
        }
    }
    FAIL() << "file-backed sequence did not reach publication";
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

TEST(TemporalAuditSequence, CompletionManifestPreservesPlanOrderExactByteHashesAndVersion) {
    const CompletionDirectory directory;
    const auto plan = Plan(TemporalAuditSpan::DayNight, 1);
    TemporalAuditSequence sequence(plan, FileBudget(plan));
    EXPECT_TRUE(sequence.completionManifest().empty());
    ReachFilePublication(sequence, plan, directory.path);
    ASSERT_EQ(sequence.next(plan.end_tick).action, Action::Publish);
    const auto text = sequence.completionManifest();
    ASSERT_FALSE(text.empty());
    const auto document = nlohmann::json::parse(text);
    EXPECT_EQ(document.at("schema"), "luminumbra.temporal_audit_sequence");
    EXPECT_EQ(document.at("version"), 1);
    EXPECT_EQ(document.at("status"), "complete");
    const auto canonical_plan = document.at("plan").dump();
    EXPECT_EQ(
        document.at("plan_sha256"),
        Luminumbra::Authoring::Detail::Sha256(
            {reinterpret_cast<const std::uint8_t*>(canonical_plan.data()), canonical_plan.size()}));
    EXPECT_EQ(document.at("plan").at("night_moon_phase_milli"), 1000);
    EXPECT_EQ(document.at("plan").at("start_tick"), plan.request.start_tick);
    EXPECT_EQ(document.at("plan").at("end_tick"), plan.end_tick);
    const auto& frames = document.at("frames");
    ASSERT_EQ(frames.size(), plan.frames.size());
    ASSERT_EQ(document.at("plan").at("frames").size(), plan.frames.size());
    for (std::size_t index = 0; index < plan.frames.size(); ++index) {
        const auto& expected = plan.frames[index];
        const auto& frame = frames.at(index);
        EXPECT_EQ(frame.at("index"), index);
        EXPECT_EQ(frame.at("tick"), expected.target_tick);
        EXPECT_EQ(frame.at("sub_tick_60"), expected.sub_tick_60);
        EXPECT_EQ(frame.at("kind"), static_cast<unsigned>(expected.kind));
        EXPECT_EQ(frame.at("control"), static_cast<unsigned>(expected.control));
        EXPECT_EQ(frame.at("dense"), expected.dense);
        EXPECT_EQ(frame.at("hold_frame"), expected.hold_frame);
        auto scheduled = frame;
        scheduled.erase("payloads");
        EXPECT_EQ(scheduled, document.at("plan").at("frames").at(index));
        const auto& payloads = frame.at("payloads");
        ASSERT_EQ(payloads.size(), index == 0 ? 2U : 1U);
        EXPECT_EQ(payloads.at(0).at("path"), "frame-" + std::to_string(index) + ".ppm");
        EXPECT_EQ(payloads.at(0).at("bytes"), 3);
        EXPECT_EQ(payloads.at(0).at("sha256"),
                  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    }
    EXPECT_EQ(frames.at(0).at("payloads").at(1).at("path"), "frame-0.ppm.depth.pfm");
    EXPECT_EQ(frames.at(0).at("payloads").at(1).at("sha256"),
              "5016d27e02b85de4602313289699acbf145b3b1e47d8f94320cce8ded4a6ceef");
    EXPECT_EQ(text, sequence.completionManifest());

    // Copies at the publication boundary exercise failures without repeating
    // capture I/O. Old output and stale staging are never replaced or removed.
    for (const auto* occupied : {"complete.json", "stale.json.tmp"}) {
        ASSERT_TRUE(WriteBytes(directory.path / occupied, "previous"));
    }
    ASSERT_TRUE(std::filesystem::create_directory(directory.path / "directory.json"));
    for (const auto& target : {directory.path / "absent" / "sequence.json",
                               directory.path / "complete.json",
                               directory.path / "stale.json",
                               directory.path / "directory.json",
                               directory.path / std::string("invalid\0.json", 13),
                               directory.path / std::string("\0ignored", 8) / "invalid-parent.json",
                               directory.path / "Upper.json",
                               directory.path / "trailing.json.",
                               directory.path / std::string(129, 'a'),
                               directory.path / "con.json",
                               directory.path / "prn.json",
                               directory.path / "aux.json",
                               directory.path / "nul.json",
                               directory.path / "com1.json",
                               directory.path / "com9.json",
                               directory.path / "lpt1.json",
                               directory.path / "lpt9.json",
                               directory.path / "",
                               directory.path / ".",
                               directory.path / "..",
                               std::filesystem::path{}}) {
        SCOPED_TRACE(target.string());
        auto failed = sequence;
        EXPECT_FALSE(failed.publishCompletionManifest(target));
        EXPECT_FALSE(failed.complete());
        EXPECT_TRUE(failed.completionManifest().empty());
        EXPECT_EQ(failed.next(plan.end_tick).action, Action::Refused);
    }
    EXPECT_FALSE(std::filesystem::exists(directory.path / "absent"));
    EXPECT_FALSE(std::filesystem::exists(directory.path / "stale.json"));
    EXPECT_FALSE(std::filesystem::exists(directory.path / "invalid"));
    EXPECT_FALSE(std::filesystem::exists(directory.path / "invalid-parent.json"));
    EXPECT_FALSE(std::filesystem::exists(directory.path / "Upper.json"));
    const CompletionDirectory other_directory;
    auto misplaced = sequence;
    EXPECT_FALSE(misplaced.publishCompletionManifest(other_directory.path / "sequence.json"));
    EXPECT_TRUE(std::filesystem::is_empty(other_directory.path));
    for (const auto* occupied : {"complete.json", "stale.json.tmp"}) {
        std::ifstream input(directory.path / occupied, std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(input), {}};
        EXPECT_EQ(bytes, "previous");
    }
    auto cancelled = sequence;
    cancelled.refuse();
    EXPECT_FALSE(cancelled.publishCompletionManifest(directory.path / "cancelled.json"));
    EXPECT_FALSE(std::filesystem::exists(directory.path / "cancelled.json"));
    EXPECT_FALSE(std::filesystem::exists(directory.path / "cancelled.json.tmp"));

    const auto manifest = directory.path / "sequence.json";
    ASSERT_TRUE(sequence.publishCompletionManifest(directory.path / "." / "sequence.json"));
    EXPECT_TRUE(sequence.complete());
    EXPECT_FALSE(std::filesystem::exists(directory.path / "sequence.json.tmp"));
    std::ifstream input(manifest, std::ios::binary);
    const std::string written{std::istreambuf_iterator<char>(input), {}};
    EXPECT_EQ(written, text);
    EXPECT_EQ(sequence.completionManifest(), text);
    EXPECT_FALSE(sequence.publishCompletionManifest(manifest));
    std::ifstream unchanged(manifest, std::ios::binary);
    const std::string retained{std::istreambuf_iterator<char>(unchanged), {}};
    EXPECT_EQ(retained, text);
}

TEST(TemporalAuditSequence, CaptureFileFailuresAndInvalidAdmissionNeverWriteCompletion) {
    const CompletionDirectory directory;
    const auto plan = Plan(TemporalAuditSpan::DayNight, 1);
    TemporalAuditSequence ready(plan, FileBudget(plan));
    auto tick = plan.request.start_tick;
    Reach(ready, Action::Capture, tick);
    ASSERT_TRUE(WriteBytes(directory.path / "frame-0.ppm", "abc"));
    ASSERT_TRUE(WriteBytes(directory.path / "frame-1.ppm", "abc"));
    ASSERT_TRUE(WriteBytes(directory.path / ("frame-0." + std::string(121, 'a')), "abc"));
    ASSERT_TRUE(WriteBytes(directory.path / "frame-0.empty", ""));
    ASSERT_TRUE(WriteBytes(directory.path / "frame-0.UPPER", "abc"));
    ASSERT_TRUE(std::filesystem::create_directory(directory.path / "frame-0.directory"));
    ASSERT_TRUE(WriteBytes(directory.path / "frame-0.large", "abc"));
    std::filesystem::resize_file(directory.path / "frame-0.large", 128ULL * 1024 * 1024 + 1);
    std::vector<std::string> too_many;
    for (unsigned index = 0; index < 13; ++index) {
        too_many.push_back("frame-0." + std::to_string(index));
        ASSERT_TRUE(WriteBytes(directory.path / too_many.back(), "abc"));
    }
    const std::vector<std::vector<std::string>> cases = {
        {},
        {"frame-0.ppm", "frame-0.ppm"},
        {"../frame-0.ppm"},
        {(directory.path / "frame-0.ppm").string()},
        {"frame-1.ppm"},
        {"frame-0.sub/path"},
        {"frame-0.missing"},
        {"frame-0.directory"},
        {"frame-0.empty"},
        {"frame-0.ppm", "frame-0.empty"},
        {"frame-0.UPPER"},
        {"frame-0.upper"}, // Windows must not record a case alias of the uppercase file.
        {"frame-0.large"},
        {std::string("frame-0.ppm\0suffix", 18)},
        {"frame-0.\xff"},
        {"frame-0.trailing."},
        {"frame-0." + std::string(121, 'a')},
        too_many};
    for (const auto& names : cases) {
        auto failed = ready;
        EXPECT_FALSE(failed.acknowledgeCaptureFiles(tick, directory.path, names));
        EXPECT_EQ(failed.captured_frames(), 0U);
        EXPECT_TRUE(failed.completionManifest().empty());
        EXPECT_FALSE(failed.publishCompletionManifest(directory.path / "refused.json"));
        EXPECT_EQ(failed.next(tick).action, Action::Refused);
    }
    std::error_code link_error;
    std::filesystem::create_symlink(
        directory.path / "frame-0.ppm", directory.path / "frame-0.link", link_error);
    if (link_error) {
        // Windows may deny symlink creation; denial must not create an input.
        EXPECT_FALSE(std::filesystem::exists(directory.path / "frame-0.link"));
    } else {
        auto linked = ready;
        const std::array<std::string, 1> link_names{"frame-0.link"};
        EXPECT_FALSE(linked.acknowledgeCaptureFiles(tick, directory.path, link_names));
        EXPECT_TRUE(linked.completionManifest().empty());
    }
    const std::array<std::string, 1> names{"frame-0.ppm"};
    for (unsigned fault = 0; fault < 6; ++fault) {
        auto failed = ready;
        const auto root = fault == 0   ? directory.path / "missing"
                          : fault == 3 ? std::filesystem::path{}
                          : fault == 4 ? directory.path / std::string("\0ignored", 8)
                          : fault == 5 ? directory.path / std::string(32768, 'x')
                                       : directory.path;
        if (fault == 2) {
            failed.refuse();
        }
        EXPECT_FALSE(failed.acknowledgeCaptureFiles(tick + (fault == 1 ? 1U : 0U), root, names));
        EXPECT_TRUE(failed.completionManifest().empty());
    }
    // A real capture acknowledgement remains incomplete after cancellation.
    auto partial = ready;
    ASSERT_TRUE(partial.acknowledgeCaptureFiles(tick, directory.path, names));
    EXPECT_EQ(partial.captured_frames(), 1U);
    EXPECT_TRUE(partial.completionManifest().empty());
    auto relocated = partial;
    auto relocated_tick = tick;
    Reach(relocated, Action::Capture, relocated_tick);
    const CompletionDirectory other_directory;
    ASSERT_TRUE(WriteBytes(other_directory.path / "frame-1.ppm", "abc"));
    const std::array<std::string, 1> relocated_names{"frame-1.ppm"};
    EXPECT_FALSE(
        relocated.acknowledgeCaptureFiles(relocated_tick, other_directory.path, relocated_names));
    EXPECT_EQ(relocated.captured_frames(), 1U);
    EXPECT_TRUE(relocated.completionManifest().empty());
    partial.refuse();
    EXPECT_FALSE(partial.publishCompletionManifest(directory.path / "partial.json"));
    EXPECT_FALSE(std::filesystem::exists(directory.path / "partial.json"));

    auto small_budget = FileBudget(plan);
    small_budget.frame_bytes = 2;
    TemporalAuditSequence tight(plan, small_budget);
    auto small_tick = plan.request.start_tick;
    Reach(tight, Action::Capture, small_tick);
    EXPECT_FALSE(tight.acknowledgeCaptureFiles(small_tick, directory.path, names));
    EXPECT_EQ(tight.captured_frames(), 0U);

    auto large_budget = FileBudget(plan);
    large_budget.frame_bytes = 128ULL * 1024 * 1024 + 1;
    large_budget.output_bytes =
        static_cast<std::uint64_t>(plan.frames.size()) * large_budget.frame_bytes +
        large_budget.manifest_bytes;
    TemporalAuditSequence oversized(plan, large_budget);
    auto oversized_tick = plan.request.start_tick;
    Reach(oversized, Action::Capture, oversized_tick);
    const std::array<std::string, 1> large_names{"frame-0.large"};
    EXPECT_FALSE(oversized.acknowledgeCaptureFiles(oversized_tick, directory.path, large_names));
    EXPECT_EQ(oversized.captured_frames(), 0U);

    // Aggregate limits apply to all payloads, not only the largest member.
    small_budget.frame_bytes = 4;
    TemporalAuditSequence aggregate(plan, small_budget);
    small_tick = plan.request.start_tick;
    Reach(aggregate, Action::Capture, small_tick);
    ASSERT_TRUE(WriteBytes(directory.path / "frame-0.second", "abc"));
    const std::array<std::string, 2> pair{"frame-0.ppm", "frame-0.second"};
    EXPECT_FALSE(aggregate.acknowledgeCaptureFiles(small_tick, directory.path, pair));
    EXPECT_EQ(aggregate.captured_frames(), 0U);

    auto duplicate = ready;
    ASSERT_TRUE(duplicate.acknowledgeCaptureFiles(tick, directory.path, names));
    EXPECT_FALSE(duplicate.acknowledgeCaptureFiles(tick, directory.path, names));
    auto mixed = ready;
    Accept(mixed, mixed.next(tick));
    auto mixed_tick = tick;
    Reach(mixed, Action::Capture, mixed_tick);
    const std::array<std::string, 1> next_names{"frame-1.ppm"};
    EXPECT_FALSE(mixed.acknowledgeCaptureFiles(mixed_tick, directory.path, next_names));

    // The inclusive filename and payload-count boundaries remain usable.
    std::vector<std::string> maximal_names{"frame-0." + std::string(120, 'a')};
    for (unsigned index = 1; index < 12; ++index) {
        maximal_names.push_back("frame-0." + std::to_string(index));
    }
    for (const auto& name : maximal_names) {
        ASSERT_TRUE(WriteBytes(directory.path / name, "a"));
    }
    auto maximal = ready;
    EXPECT_TRUE(maximal.acknowledgeCaptureFiles(tick, directory.path, maximal_names));
    EXPECT_EQ(maximal.captured_frames(), 1U);

    TemporalAuditSequence invalid(plan, {});
    EXPECT_FALSE(invalid.publishCompletionManifest(directory.path / "uncreated" / "result.json"));
    EXPECT_FALSE(std::filesystem::exists(directory.path / "uncreated"));
    TemporalAuditSequence early(plan, FileBudget(plan));
    EXPECT_FALSE(early.publishCompletionManifest(directory.path / "early.json"));
    EXPECT_FALSE(std::filesystem::exists(directory.path / "early.json"));
    EXPECT_FALSE(std::filesystem::exists(directory.path / "refused.json"));
    EXPECT_FALSE(std::filesystem::exists(directory.path / "refused.json.tmp"));
}

TEST(TemporalAuditSequence, PublicationRequiresFileAcknowledgementsAndManifestReservation) {
    const CompletionDirectory directory;
    const auto plan = Plan(TemporalAuditSpan::DayNight, 1);
    TemporalAuditSequence unverified(plan, FileBudget(plan));
    auto tick = plan.request.start_tick;
    Reach(unverified, Action::Publish, tick);
    EXPECT_TRUE(unverified.completionManifest().empty());
    EXPECT_FALSE(unverified.publishCompletionManifest(directory.path / "unverified.json"));
    EXPECT_FALSE(std::filesystem::exists(directory.path / "unverified.json"));
    EXPECT_FALSE(std::filesystem::exists(directory.path / "unverified.json.tmp"));

    auto small_budget = FileBudget(plan);
    small_budget.manifest_bytes = 1;
    TemporalAuditSequence tight(plan, small_budget);
    ReachFilePublication(tight, plan, directory.path);
    ASSERT_EQ(tight.next(plan.end_tick).action, Action::Publish);
    EXPECT_TRUE(tight.completionManifest().empty());
    EXPECT_FALSE(tight.publishCompletionManifest(directory.path / "tight.json"));
    EXPECT_FALSE(std::filesystem::exists(directory.path / "tight.json"));
    EXPECT_FALSE(std::filesystem::exists(directory.path / "tight.json.tmp"));
}
} // namespace
