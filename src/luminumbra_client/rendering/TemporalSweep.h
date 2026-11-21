#pragma once

#include "rendering/CaptureFeatureProfile.h"
#include "rendering/TemporalAudit.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Luminumbra::Rendering {

// Shared with the still renderer. Changing these pins changes the standing
// still-matrix scenes; temporal samples instead use the admitted tick plan.
struct WorldSweepTime {
    const char* label;
    float time_of_day;
    constexpr bool operator==(const WorldSweepTime& other) const {
        return std::string_view(label) == other.label && time_of_day == other.time_of_day;
    }
};
struct WorldSweepAngle {
    const char* label;
    float yaw_degrees;
    float pitch_degrees;
    bool aim_at_water;
    const char* feature;
    constexpr bool operator==(const WorldSweepAngle& other) const {
        return std::string_view(label) == other.label && yaw_degrees == other.yaw_degrees &&
               pitch_degrees == other.pitch_degrees && aim_at_water == other.aim_at_water &&
               std::string_view(feature) == other.feature;
    }
};
inline constexpr std::array<WorldSweepTime, 4> kWorldSweepTimes{
    {{"dawn", 0.17f}, {"noon", 0.0f}, {"dusk", 0.25f}, {"night", 0.5f}}};
inline constexpr std::array<WorldSweepAngle, 6> kWorldSweepAngles{
    {{"yaw000", 0.0f, 0.0f, false, "horizon_vista"},
     {"yaw120", 120.0f, 0.0f, false, "horizon_vista"},
     {"yaw240", 240.0f, 0.0f, false, "horizon_vista"},
     {"down35", 45.0f, -35.0f, false, "ground_foliage"},
     {"up25", 200.0f, 25.0f, false, "sky_clouds"},
     {"water", 0.0f, 0.0f, true, "water_shore"}}};
inline constexpr std::array<const char*, 2> kWorldSweepWeather{{"clear", "storm"}};
inline constexpr std::string_view kWorldSweepFoliageRefusal =
    "sweep_upward_camera_forces_empty_foliage";

inline constexpr int WorldSweepStillSettleFrames(bool storm) {
    return storm ? 5 : 3;
}
inline constexpr bool WorldSweepClearsFoliage(float pitch_degrees) {
    return pitch_degrees > 5.0f;
}

// An entire legacy matrix necessarily includes the forced-empty upward view.
// Startup uses this before world/save-root/output creation, including env launch.
inline constexpr std::string_view WorldSweepMatrixRefusal(bool all_features) {
    return all_features ? kWorldSweepFoliageRefusal : std::string_view{};
}

inline std::string_view TemporalSweepRefusal(const CaptureFeatureProfile& profile,
                                             std::size_t camera_index) {
    if (camera_index >= kWorldSweepAngles.size()) {
        return "sweep_unknown_camera";
    }
    const auto& foliage = profile.features()[static_cast<std::size_t>(CaptureFeature::Foliage)];
    if (foliage.requested &&
        WorldSweepClearsFoliage(kWorldSweepAngles[camera_index].pitch_degrees)) {
        return kWorldSweepFoliageRefusal;
    }
    if (profile.capability() != CaptureFeatureCapability::Supported) {
        return "sweep_feature_profile_unavailable";
    }
    if (CaptureFeaturePolicy::Resolve(&profile).UseLegacyAtmospherePins()) {
        return "sweep_feature_profile_incomplete";
    }
    return {};
}

struct TemporalSweepOutput {
    bool success = false;
    std::uint64_t bytes = 0;
};

// GL-free fixed-camera sweep adapter. One transport and one sequence own all
// samples: there is deliberately no per-cell initialization/reset callback.
// advance_tick runs normal simulation once; render consumes the exact step and
// shared feature policy (including zero dt on readiness retries). Output remains
// private to the transport until publish succeeds. Runtime GL transport and
// startup admission are separate prerequisites, not supplied by this protocol.
// A null profile retains legacy producer policy; it cannot admit all-features.
// A nonempty result requires the caller to abandon its transport reservation.
// No type-erased per-frame calls. The admitted plan bounds every operation by
// ticks + render attempts + captures + one publication; each step is O(1).
template<typename Transport>
std::string RunTemporalSweep(const TemporalAuditPlan& plan,
                             const TemporalSequenceBudget& budget,
                             const CaptureFeatureProfile* profile,
                             std::size_t camera_index,
                             Transport& transport) {
    const auto refusal = profile != nullptr ? TemporalSweepRefusal(*profile, camera_index)
                                            : (camera_index < kWorldSweepAngles.size()
                                                   ? std::string_view{}
                                                   : std::string_view("sweep_unknown_camera"));
    if (!refusal.empty()) {
        return std::string(refusal);
    }
    TemporalAuditSequence sequence(plan, budget);
    // Validate the whole plan/budget before even querying the transport clock.
    if (sequence.next(plan.request.start_tick).action == TemporalSequenceAction::Refused) {
        return std::string(sequence.refusal());
    }
    const auto policy = CaptureFeaturePolicy::Resolve(profile);
    const auto bound =
        plan.end_tick - plan.request.start_tick + plan.max_render_attempts + plan.frames.size() + 1;
    for (std::uint64_t attempt = 0; attempt < bound; ++attempt) {
        const auto command = sequence.next(transport.actual_tick());
        TemporalSweepOutput output;
        bool success = true;
        bool ready = true;
        switch (command.action) {
            case TemporalSequenceAction::Refused:
                return std::string(sequence.refusal());
            case TemporalSequenceAction::Complete:
                return {};
            case TemporalSequenceAction::AdvanceTick:
                success = transport.advance_tick(command);
                break;
            case TemporalSequenceAction::Capture:
                output = transport.capture(command);
                success = output.success;
                break;
            case TemporalSequenceAction::Publish:
                output = transport.publish(command);
                success = output.success;
                break;
            default:
                success = transport.render(command, policy, kWorldSweepAngles[camera_index], ready);
                break;
        }
        sequence.acknowledge(transport.actual_tick(), success, ready, output.bytes);
        if (!sequence.refusal().empty()) {
            return std::string(sequence.refusal());
        }
        if (sequence.complete()) {
            return {};
        }
    }
    return "sweep_step_budget_exhausted";
}

// Preserve the feature-qualified adapter contract for existing callers.
template<typename Transport>
std::string RunTemporalSweep(const TemporalAuditPlan& plan,
                             const TemporalSequenceBudget& budget,
                             const CaptureFeatureProfile& profile,
                             std::size_t camera_index,
                             Transport& transport) {
    return RunTemporalSweep(plan, budget, &profile, camera_index, transport);
}

} // namespace Luminumbra::Rendering
