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
} // namespace Luminumbra::Rendering
