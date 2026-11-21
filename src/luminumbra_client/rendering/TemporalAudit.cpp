#include "rendering/TemporalAudit.h"
#include "luminumbra_common/world/WorldClock.h"

#include <algorithm>
#include <utility>

namespace Luminumbra::Rendering {
namespace {
constexpr std::uint32_t kMaxTickRate = 1000;

std::uint64_t CeilDivide(std::uint64_t value, std::uint64_t divisor) {
    return value / divisor + (value % divisor != 0 ? 1 : 0);
}

std::string Validate(const TemporalAuditRequest& request) {
    if (request.span != TemporalAuditSpan::DayNight &&
        request.span != TemporalAuditSpan::WeatherCycle &&
        request.span != TemporalAuditSpan::MultiDay)
        return "unknown temporal span";
    if (request.tick_limit == 0 || request.tick_limit > kTemporalMaxTicks)
        return "tick limit must be 1..10000000";
    if (request.frame_limit == 0 || request.frame_limit > kTemporalMaxFrames)
        return "frame limit must be 1..4096";
    if (request.ticks_per_second == 0 || request.ticks_per_second > kMaxTickRate)
        return "tick rate must be 1..1000 Hz";
    if (request.ticks_per_day == 0 || request.ticks_per_day > kTemporalMaxTicks)
        return "ticks per day must be 1..10000000";
    if (request.start_tick >= world::WorldClock::kTickLimit)
        return "start tick exceeds simulation clock range";
    if (request.dawn_tick == 0 || request.dawn_tick >= request.dusk_tick ||
        request.dusk_tick >= request.ticks_per_day)
        return "calendar crossings require 0 < dawn < dusk < ticks per day";
    if (request.night_moon_phase_milli > 1000)
        return "moon phase must be 0..1000";
    if (request.span == TemporalAuditSpan::WeatherCycle) {
        if (request.strike_tick < 50ULL * request.ticks_per_second ||
            request.strike_tick >= 90ULL * request.ticks_per_second)
            return "strike tick must be in storm interval [50T,90T)";
    } else if (request.strike_tick != 0) {
        return "strike tick requires weather-cycle span";
    }
    return {};
}

std::vector<std::uint64_t> BurstAnchors(const TemporalAuditRequest& request,
                                        std::uint64_t duration) {
    std::vector<std::uint64_t> anchors;
    // At most eight calendar crossings, or six weather anchors.
    anchors.reserve(8);
    if (request.span == TemporalAuditSpan::WeatherCycle) {
        for (const auto seconds : {0U, 20U, 50U, 90U, 120U})
            anchors.push_back(std::uint64_t{seconds} * request.ticks_per_second);
        anchors.push_back(request.strike_tick);
    } else {
        const auto day_phase = request.start_tick % request.ticks_per_day;
        for (const auto crossing : {request.dawn_tick, request.dusk_tick}) {
            const auto first = crossing >= day_phase
                                   ? crossing - day_phase
                                   : request.ticks_per_day - (day_phase - crossing);
            for (auto offset = first; offset <= duration; offset += request.ticks_per_day)
                anchors.push_back(offset);
        }
    }
    return anchors;
}

void AddHolds(TemporalAuditPlan& plan) {
    const bool weather = plan.request.span == TemporalAuditSpan::WeatherCycle;
    for (const auto kind :
         {TemporalAuditFrameKind::SteadyHold, TemporalAuditFrameKind::NightPresenceHold}) {
        if (kind == TemporalAuditFrameKind::NightPresenceHold && !weather)
            continue;
        const auto control =
            kind == TemporalAuditFrameKind::NightPresenceHold
                ? TemporalAuditControl::Night
                : (weather ? TemporalAuditControl::Noon : TemporalAuditControl::Live);
        for (std::uint32_t index = 0; index < kTemporalBurstFrames; ++index)
            plan.frames.push_back({.target_tick = plan.end_tick,
                                   .sub_tick_60 = 0,
                                   .kind = kind,
                                   .control = control,
                                   .dense = true,
                                   .hold_frame = index});
    }
}
} // namespace

TemporalAuditAdmission BuildTemporalAuditPlan(const TemporalAuditRequest& request) {
    if (const auto reason = Validate(request); !reason.empty())
        return {.plan = std::nullopt, .reason = reason};
    const bool weather = request.span == TemporalAuditSpan::WeatherCycle;
    // Inputs have already been bounded: all following products fit uint64_t.
    const std::uint64_t duration =
        weather ? 120ULL * request.ticks_per_second
                : request.ticks_per_day * (request.span == TemporalAuditSpan::MultiDay ? 3 : 1);
    if (duration > request.tick_limit)
        return {.plan = std::nullopt, .reason = "span exceeds tick budget"};
    if (duration >= world::WorldClock::kTickLimit - request.start_tick)
        return {.plan = std::nullopt, .reason = "end tick exceeds simulation clock range"};
    const auto burst_extent = CeilDivide(119ULL * request.ticks_per_second, 60);
    if (duration < burst_extent)
        return {.plan = std::nullopt, .reason = "span cannot contain a complete 120-frame burst"};

    TemporalAuditPlan plan{
        .request = request,
        .end_tick = request.start_tick + duration,
        .sparse_interval_ticks =
            weather ? request.ticks_per_second
                    : CeilDivide(request.ticks_per_day,
                                 request.span == TemporalAuditSpan::MultiDay ? 240 : 720),
        .boundary_ticks = {},
        .frames = {},
        .settling = {},
        .presentation_steps = 0,
        .max_render_attempts = 0};
    const auto anchors = BurstAnchors(request, duration);
    if (weather) {
        for (const auto seconds : {0U, 20U, 50U, 90U, 120U})
            plan.boundary_ticks.push_back(request.start_tick +
                                          std::uint64_t{seconds} * request.ticks_per_second);
    } else {
        // Include exact elapsed-day endpoints and actual calendar midnights.
        for (std::uint64_t offset = 0; offset <= duration; offset += request.ticks_per_day)
            plan.boundary_ticks.push_back(request.start_tick + offset);
        const auto until_midnight =
            request.ticks_per_day - request.start_tick % request.ticks_per_day;
        for (auto offset = until_midnight; offset <= duration; offset += request.ticks_per_day)
            plan.boundary_ticks.push_back(request.start_tick + offset);
    }
    plan.boundary_ticks.push_back(plan.end_tick);
    std::ranges::sort(plan.boundary_ticks);
    const auto duplicate_boundaries = std::ranges::unique(plan.boundary_ticks);
    plan.boundary_ticks.erase(duplicate_boundaries.begin(), duplicate_boundaries.end());

    // No tick-length traversal: <= 721 sparse candidates, 8 boundaries and
    // 8 * (120 burst candidates + one anchor) plus 240 holds, bounded before allocation.
    const auto sparse_count = duration / plan.sparse_interval_ticks + 1;
    const auto hold_count = weather ? 240U : 120U;
    constexpr std::size_t max_candidates = 721 + 8 + 8 * (kTemporalBurstFrames + 1) + 240;
    static_assert(max_candidates <= kTemporalMaxFrames);
    const auto candidate_count = sparse_count + plan.boundary_ticks.size() +
                                 anchors.size() * (kTemporalBurstFrames + 1) + hold_count;
    plan.frames.reserve(static_cast<std::size_t>(candidate_count));
    const auto control = weather ? TemporalAuditControl::Noon : TemporalAuditControl::Live;
    const auto append = [&](std::uint64_t offset, std::uint32_t phase, bool dense) {
        plan.frames.push_back({.target_tick = request.start_tick + offset,
                               .sub_tick_60 = phase,
                               .kind = TemporalAuditFrameKind::Sequence,
                               .control = control,
                               .dense = dense,
                               .hold_frame = 0});
    };
    for (std::uint64_t offset = 0; offset <= duration; offset += plan.sparse_interval_ticks)
        append(offset, 0, false);
    for (const auto tick : plan.boundary_ticks)
        append(tick - request.start_tick, 0, false);
    for (const auto anchor : anchors) {
        // Center on the event when possible; shift endpoint windows inside span.
        const auto start =
            std::min(anchor > request.ticks_per_second ? anchor - request.ticks_per_second : 0,
                     duration - burst_extent);
        append(anchor, 0, false);
        for (std::uint32_t index = 0; index < kTemporalBurstFrames; ++index) {
            const auto numerator = std::uint64_t{index} * request.ticks_per_second;
            append(start + numerator / 60, static_cast<std::uint32_t>(numerator % 60), true);
        }
    }
    std::ranges::sort(plan.frames, [](const auto& left, const auto& right) {
        if (left.target_tick != right.target_tick)
            return left.target_tick < right.target_tick;
        if (left.sub_tick_60 != right.sub_tick_60)
            return left.sub_tick_60 < right.sub_tick_60;
        return left.dense > right.dense;
    });
    const auto duplicates =
        std::ranges::unique(plan.frames, [](const auto& left, const auto& right) {
            return left.target_tick == right.target_tick && left.sub_tick_60 == right.sub_tick_60;
        });
    plan.frames.erase(duplicates.begin(), duplicates.end());
    AddHolds(plan);
    if (plan.frames.size() > request.frame_limit)
        return {.plan = std::nullopt, .reason = "span exceeds frame budget"};
    plan.presentation_steps = CeilDivide(duration * 60, request.ticks_per_second) + hold_count;
    // <= 600 million presentation steps + 262144 sample attempts + warmup.
    plan.max_render_attempts = plan.presentation_steps + plan.settling.warmup_frames +
                               plan.settling.initial_readiness_retries +
                               plan.frames.size() * (1 + plan.settling.sample_settle_frames +
                                                     plan.settling.sample_readiness_retries);
    return {.plan = std::move(plan), .reason = {}};
}

std::optional<std::uint64_t> TemporalAuditTicksToFrame(const TemporalAuditPlan& plan,
                                                       std::size_t frame_index,
                                                       std::uint64_t current_tick) {
    if (frame_index >= plan.frames.size() || current_tick < plan.request.start_tick)
        return std::nullopt;
    const auto target = plan.frames[frame_index].target_tick;
    if (current_tick > target)
        return std::nullopt;
    return target - current_tick;
}

TemporalAuditSequence::TemporalAuditSequence(const TemporalAuditPlan& plan,
                                             const TemporalSequenceBudget& budget)
    : budget_(budget) {
    // Re-admit the entire immutable schedule, including settling/work bounds.
    // At most 4096 frames are retained; caller-edited plans cannot raise limits.
    auto admitted = BuildTemporalAuditPlan(plan.request);
    if (!admitted.plan || *admitted.plan != plan) {
        fail("sequence requires an unmodified admitted plan");
        return;
    }
    if (budget.frame_bytes == 0 || budget.manifest_bytes == 0 || budget.output_bytes == 0 ||
        budget.output_bytes > kTemporalMaxOutputBytes ||
        budget.manifest_bytes > budget.output_bytes ||
        plan.frames.size() > (budget.output_bytes - budget.manifest_bytes) / budget.frame_bytes) {
        fail("sequence exceeds output budget");
        return;
    }
    plan_ = std::move(*admitted.plan);
    current_tick_ = plan_.request.start_tick;
    stage_ = TemporalSequenceAction::Warmup;
}

void TemporalAuditSequence::fail(std::string_view reason) {
    if (stage_ != TemporalSequenceAction::Refused || refusal_.empty()) {
        refusal_ = reason;
    }
    stage_ = TemporalSequenceAction::Refused;
    pending_.reset();
}

void TemporalAuditSequence::refuse() {
    fail("sequence transport refused");
}

TemporalSequenceStep TemporalAuditSequence::step(TemporalSequenceAction action,
                                                 std::uint64_t tick,
                                                 std::uint64_t phase_60,
                                                 double delta) const {
    const auto& frame = plan_.frames[std::min(frame_index_, plan_.frames.size() - 1)];
    const auto calendar_tick =
        (plan_.request.start_tick + phase_60 / 60) % plan_.request.ticks_per_day;
    const double day_phase =
        (static_cast<double>(calendar_tick) + static_cast<double>(phase_60 % 60) / 60.0) /
        static_cast<double>(plan_.request.ticks_per_day);
    return {.action = action,
            .simulation_tick = tick,
            .sub_tick_60 = static_cast<std::uint32_t>(phase_60 % 60),
            .frame_index = frame_index_,
            .presentation_delta_seconds = delta,
            .day_phase = day_phase,
            .control = frame.control,
            .byte_limit = action == TemporalSequenceAction::Capture   ? budget_.frame_bytes
                          : action == TemporalSequenceAction::Publish ? budget_.manifest_bytes
                                                                      : 0};
}

void TemporalAuditSequence::beginSample() {
    attempts_ = 0;
    stage_ = plan_.frames[frame_index_].dense ? TemporalSequenceAction::Readiness
                                              : TemporalSequenceAction::Settle;
}

void TemporalAuditSequence::beginFrame() {
    attempts_ = 0;
    if (frame_index_ == plan_.frames.size()) {
        stage_ = TemporalSequenceAction::Publish;
        return;
    }
    const auto& frame = plan_.frames[frame_index_];
    const auto target = (frame.target_tick - plan_.request.start_tick) * 60 + frame.sub_tick_60;
    if (frame.kind == TemporalAuditFrameKind::Sequence && target == phase_60_) {
        beginSample();
    } else {
        stage_ = TemporalSequenceAction::Present;
    }
}

TemporalSequenceStep TemporalAuditSequence::next(std::uint64_t actual_tick) {
    if (stage_ == TemporalSequenceAction::Refused) {
        return {};
    }
    if (actual_tick != current_tick_) {
        fail("sequence simulation clock changed outside schedule");
        return {};
    }
    if (pending_) {
        return *pending_;
    }
    auto command = step(stage_, current_tick_, phase_60_, 0);
    if (stage_ == TemporalSequenceAction::Complete) {
        return command;
    }
    if (stage_ == TemporalSequenceAction::Warmup) {
        command.presentation_delta_seconds = 1.0 / kTemporalPresentationHz;
    } else if (stage_ == TemporalSequenceAction::Present) {
        const auto& frame = plan_.frames[frame_index_];
        if (frame.kind == TemporalAuditFrameKind::Sequence) {
            const auto target =
                (frame.target_tick - plan_.request.start_tick) * 60 + frame.sub_tick_60;
            next_phase_60_ = std::min(phase_60_ + plan_.request.ticks_per_second, target);
            const auto next_tick = plan_.request.start_tick + next_phase_60_ / 60;
            if (current_tick_ < next_tick) {
                command = step(TemporalSequenceAction::AdvanceTick,
                               current_tick_ + 1,
                               (current_tick_ + 1 - plan_.request.start_tick) * 60,
                               0);
            } else {
                command = step(stage_,
                               current_tick_,
                               next_phase_60_,
                               static_cast<double>(next_phase_60_ - phase_60_) /
                                   (60.0 * plan_.request.ticks_per_second));
            }
        } else {
            // Each hold starts at frame zero at the final simulation tick.
            next_phase_60_ = phase_60_;
            command.presentation_delta_seconds =
                frame.hold_frame == 0 ? 0.0 : 1.0 / kTemporalPresentationHz;
        }
    }
    const bool render = command.action != TemporalSequenceAction::AdvanceTick &&
                        command.action != TemporalSequenceAction::Capture &&
                        command.action != TemporalSequenceAction::Publish;
    if (render && render_attempts_ >= plan_.max_render_attempts) {
        fail("sequence render budget exhausted");
        return {};
    }
    pending_ = command;
    return command;
}

void TemporalAuditSequence::acknowledge(std::uint64_t actual_tick,
                                        bool success,
                                        bool ready,
                                        std::uint64_t written_bytes) {
    if (stage_ == TemporalSequenceAction::Refused) {
        return;
    }
    if (!pending_) {
        fail("sequence acknowledgement without a pending step");
        return;
    }
    const auto command = *pending_;
    pending_.reset();
    if (!success || actual_tick != command.simulation_tick) {
        fail("sequence step failed or simulation tick stalled");
        return;
    }
    const bool output = command.action == TemporalSequenceAction::Capture ||
                        command.action == TemporalSequenceAction::Publish;
    if ((output && (written_bytes == 0 || written_bytes > command.byte_limit)) ||
        (!output && written_bytes != 0)) {
        fail("sequence transport exceeded reservation or returned empty output");
        return;
    }
    switch (command.action) {
        case TemporalSequenceAction::AdvanceTick:
            current_tick_ = actual_tick;
            return;
        case TemporalSequenceAction::Warmup:
            ++render_attempts_;
            if (++attempts_ == plan_.settling.warmup_frames) {
                attempts_ = 0;
                stage_ = TemporalSequenceAction::InitialReadiness;
            }
            return;
        case TemporalSequenceAction::InitialReadiness:
        case TemporalSequenceAction::Readiness: {
            ++render_attempts_;
            if (ready) {
                if (command.action == TemporalSequenceAction::InitialReadiness) {
                    beginFrame();
                } else {
                    stage_ = TemporalSequenceAction::Capture;
                }
            } else {
                const auto limit = command.action == TemporalSequenceAction::InitialReadiness
                                       ? plan_.settling.initial_readiness_retries
                                       : plan_.settling.sample_readiness_retries;
                if (++attempts_ == limit) {
                    fail("sequence readiness exhausted");
                }
            }
            return;
        }
        case TemporalSequenceAction::Present: {
            ++render_attempts_;
            phase_60_ = next_phase_60_;
            const auto& frame = plan_.frames[frame_index_];
            const auto target =
                (frame.target_tick - plan_.request.start_tick) * 60 + frame.sub_tick_60;
            if (frame.kind != TemporalAuditFrameKind::Sequence || phase_60_ == target) {
                beginSample();
            }
            return;
        }
        case TemporalSequenceAction::Settle:
            ++render_attempts_;
            if (++attempts_ == plan_.settling.sample_settle_frames) {
                attempts_ = 0;
                stage_ = TemporalSequenceAction::Readiness;
            }
            return;
        case TemporalSequenceAction::Capture:
            ++frame_index_;
            beginFrame();
            return;
        case TemporalSequenceAction::Publish:
            stage_ = TemporalSequenceAction::Complete;
            return;
        case TemporalSequenceAction::Complete:
        case TemporalSequenceAction::Refused:
            fail("invalid sequence acknowledgement");
            return;
    }
}
} // namespace Luminumbra::Rendering
