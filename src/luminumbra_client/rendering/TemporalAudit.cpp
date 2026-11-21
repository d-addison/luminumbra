#include "rendering/TemporalAudit.h"
#include "luminumbra_common/authoring/PrefabDigest.h"
#include "luminumbra_common/core/CaptureContentPaths.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/persistence/CreatedWorldArtifacts.h"
#include "luminumbra_common/world/WorldClock.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace Luminumbra::Rendering {
namespace {
constexpr std::uint32_t kMaxTickRate = 1000;
constexpr std::size_t kMaxFramePayloads = 12;
constexpr std::uint64_t kMaxPayloadBytes = 128ULL * 1024 * 1024;
constexpr std::size_t kMaxSequenceManifestBytes = 16U * 1024 * 1024;

std::string Sha256(std::string_view bytes) {
    return Authoring::Detail::Sha256(
        {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
}

bool BoundedPath(const std::filesystem::path& path) {
    // Match the verified reader's 32767-native-unit bound before copying paths.
    return !path.empty() && path.native().size() <= 32767 &&
           path.native().find(std::filesystem::path::value_type{}) ==
               std::filesystem::path::string_type::npos;
}

bool AsciiLeaf(std::string_view name) {
    // Lowercase ASCII leaves avoid encoding, traversal and case aliases on all hosts.
    if (name.empty() || name.size() > 128 || name.back() == '.' ||
        !std::ranges::all_of(name, [](char character) {
            return (character >= 'a' && character <= 'z') ||
                   (character >= '0' && character <= '9') || character == '-' || character == '_' ||
                   character == '.';
        })) {
        return false;
    }
    const auto stem = name.substr(0, name.find('.'));
    const bool numbered_device = stem.size() == 4 &&
                                 (stem.starts_with("com") || stem.starts_with("lpt")) &&
                                 stem.back() >= '1' && stem.back() <= '9';
    return stem != "con" && stem != "prn" && stem != "aux" && stem != "nul" && !numbered_device;
}

bool PayloadName(std::string_view name, std::size_t frame_index) {
    const auto prefix = "frame-" + std::to_string(frame_index) + ".";
    return name.size() > prefix.size() && name.starts_with(prefix) && AsciiLeaf(name);
}

std::string ReadPayload(const Filesystem::VerifiedFileInput& directory,
                        const std::string& filename,
                        std::uint64_t remaining) {
    Filesystem::VerifiedFileInput input(directory.resolved_path() / filename);
    if (!Filesystem::PathSpellingEqual(input.resolved_path(),
                                       directory.resolved_path() / filename) ||
        input.resolved_path().filename() != std::filesystem::path(filename)) {
        // Preserve the actual leaf spelling for artifacts moved between hosts.
        throw std::runtime_error("Temporal payload escaped its admitted directory");
    }
    const auto size = input.size();
    if (size == 0 || size > remaining || size > kMaxPayloadBytes) {
        throw std::runtime_error("Temporal payload exceeded its reservation or was empty");
    }
    // One payload at a time, <= 128 MiB, plus the reader's fixed 64 KiB block.
    std::string bytes(static_cast<std::size_t>(size), '\0');
    std::array<char, 65536> block{};
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto received = input.read(block);
        if (received == 0 || received > bytes.size() - offset) {
            throw std::runtime_error("Temporal payload changed while reading");
        }
        std::copy_n(block.data(), received, bytes.data() + offset);
        offset += received;
    }
    if (input.read(block) != 0) {
        throw std::runtime_error("Temporal payload grew while reading");
    }
    return bytes;
}

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

bool TemporalAuditSequence::acknowledgeCaptureFiles(std::uint64_t actual_tick,
                                                    const std::filesystem::path& directory,
                                                    std::span<const std::string> filenames) try {
    if (!pending_ || pending_->action != TemporalSequenceAction::Capture ||
        actual_tick != pending_->simulation_tick || payloads_.size() != frame_index_ ||
        filenames.empty() || filenames.size() > kMaxFramePayloads || !BoundedPath(directory)) {
        fail("sequence invalid capture payload acknowledgement");
        return false;
    }
    // Validate all names before opening anything. The pairwise pass is bounded
    // to 12 * 12 comparisons; frame prefixes prevent reuse across frames.
    for (std::size_t index = 0; index < filenames.size(); ++index) {
        if (!PayloadName(filenames[index], frame_index_) ||
            std::find(filenames.begin(),
                      filenames.begin() + static_cast<std::ptrdiff_t>(index),
                      filenames[index]) != filenames.begin() + static_cast<std::ptrdiff_t>(index)) {
            fail("sequence invalid or duplicate payload name");
            return false;
        }
    }
    const Filesystem::VerifiedFileInput parent(directory,
                                               Filesystem::VerifiedFileInput::Kind::Directory);
    if (!payload_directory_.empty() &&
        !Filesystem::PathSpellingEqual(parent.resolved_path(), payload_directory_)) {
        fail("sequence capture directory changed");
        return false;
    }
    std::vector<Payload> captured;
    captured.reserve(filenames.size());
    std::uint64_t total = 0;
    for (const auto& filename : filenames) {
        const auto bytes = ReadPayload(parent, filename, pending_->byte_limit - total);
        captured.push_back({.path = filename, .sha256 = Sha256(bytes), .bytes = bytes.size()});
        total += bytes.size();
    }
    if (payloads_.empty()) {
        payloads_.reserve(plan_.frames.size());
        payload_directory_ = parent.resolved_path();
    }
    payloads_.push_back(std::move(captured));
    acknowledge(actual_tick, true, true, total);
    return stage_ != TemporalSequenceAction::Refused;
} catch (const std::exception&) {
    fail("sequence payload read or hash failed");
    return false;
}

std::string TemporalAuditSequence::completionManifest() const {
    if ((stage_ != TemporalSequenceAction::Publish && stage_ != TemporalSequenceAction::Complete) ||
        payloads_.size() != plan_.frames.size() || frame_index_ != plan_.frames.size()) {
        return {};
    }
    const auto& request = plan_.request;
    nlohmann::json plan = {
        {"span", static_cast<unsigned>(request.span)},
        {"start_tick", request.start_tick},
        {"ticks_per_day", request.ticks_per_day},
        {"ticks_per_second", request.ticks_per_second},
        {"dawn_tick", request.dawn_tick},
        {"dusk_tick", request.dusk_tick},
        {"strike_tick", request.strike_tick},
        {"night_moon_phase_milli", request.night_moon_phase_milli},
        {"tick_limit", request.tick_limit},
        {"frame_limit", request.frame_limit},
        {"end_tick", plan_.end_tick},
        {"sparse_interval_ticks", plan_.sparse_interval_ticks},
        {"boundary_ticks", plan_.boundary_ticks},
        {"presentation_steps", plan_.presentation_steps},
        {"max_render_attempts", plan_.max_render_attempts},
        {"settling",
         {{"warmup_frames", plan_.settling.warmup_frames},
          {"initial_readiness_retries", plan_.settling.initial_readiness_retries},
          {"sample_settle_frames", plan_.settling.sample_settle_frames},
          {"sample_readiness_retries", plan_.settling.sample_readiness_retries}}},
        {"frames", nlohmann::json::array()}};
    // At most 4096 frames * 12 payloads with 128-byte ASCII paths. Both JSON
    // documents and serialized text are bounded independently of payload size.
    auto frames = nlohmann::json::array();
    for (std::size_t index = 0; index < plan_.frames.size(); ++index) {
        const auto& frame = plan_.frames[index];
        nlohmann::json scheduled = {{"index", index},
                                    {"tick", frame.target_tick},
                                    {"sub_tick_60", frame.sub_tick_60},
                                    {"kind", static_cast<unsigned>(frame.kind)},
                                    {"control", static_cast<unsigned>(frame.control)},
                                    {"dense", frame.dense},
                                    {"hold_frame", frame.hold_frame}};
        plan["frames"].push_back(scheduled);
        scheduled["payloads"] = nlohmann::json::array();
        for (const auto& payload : payloads_[index]) {
            scheduled["payloads"].push_back(
                {{"path", payload.path}, {"sha256", payload.sha256}, {"bytes", payload.bytes}});
        }
        frames.push_back(std::move(scheduled));
    }
    const nlohmann::json document = {{"schema", "luminumbra.temporal_audit_sequence"},
                                     {"version", 1},
                                     {"status", "complete"},
                                     {"plan_sha256", Sha256(plan.dump())},
                                     {"plan", std::move(plan)},
                                     {"frames", std::move(frames)}};
    auto text = document.dump() + "\n";
    if (text.size() > kMaxSequenceManifestBytes || text.size() > budget_.manifest_bytes) {
        return {};
    }
    return text;
}

bool TemporalAuditSequence::publishCompletionManifest(const std::filesystem::path& path) try {
    if (!pending_ || pending_->action != TemporalSequenceAction::Publish || !BoundedPath(path)) {
        fail("sequence invalid manifest publication");
        return false;
    }
    if (!AsciiLeaf(Filesystem::GenericUtf8PathString(path.filename()))) {
        fail("sequence invalid manifest name");
        return false;
    }
    const auto text = completionManifest();
    if (text.empty()) {
        fail("sequence manifest missing acknowledgements or exceeds reservation");
        return false;
    }
    const auto parent_path =
        path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    const Filesystem::VerifiedFileInput parent(parent_path,
                                               Filesystem::VerifiedFileInput::Kind::Directory);
    if (!Filesystem::PathSpellingEqual(parent.resolved_path(), payload_directory_)) {
        fail("sequence manifest is outside the capture directory");
        return false;
    }
    // Use the same identity-checked, exclusive publication journal as A4.
    // A borrowed parent is never created or removed. Cleanup only retires
    // identities created by this publication.
    Persistence::CreatedWorldArtifacts publication;
    publication.bind_existing_root(parent.resolved_path());
    const auto destination = parent.resolved_path() / path.filename();
    publication.reserve_file(destination);
    auto temporary = destination;
    temporary += ".tmp";
    try {
        publication.write_file(temporary, text);
        publication.publish_file(temporary, destination);
    } catch (const std::exception&) {
        // Windows can report a flush failure after binding the destination.
        // Retire only this call's identities, including any published alias.
        // Unowned/replaced entries remain untouched on cleanup refusal.
        publication.rollback();
        throw;
    }
    publication.release();
    acknowledge(current_tick_, true, true, text.size());
    return complete();
} catch (const std::exception&) {
    fail("sequence manifest publication failed");
    return false;
}
} // namespace Luminumbra::Rendering
