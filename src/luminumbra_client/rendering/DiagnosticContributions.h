#pragma once

#include "DiagnosticDrawTable.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace Luminumbra::Rendering {

enum class ForwardFeature : std::uint8_t {
    WaterSurface,
    River,
    Waterfall,
    Particles,
    Spray,
    Foliage,
    Count
};
inline constexpr std::size_t kForwardFeatureCount = static_cast<std::size_t>(ForwardFeature::Count);
enum class ForwardPresenceStatus {
    Present,
    Absent,
    Occluded,
    Disabled,
    Unsupported
};
struct ForwardFeatureEvidence {
    std::uint32_t submitted_draws = 0;
    std::uint32_t visible_draws = 0;
    std::uint64_t contributor_samples = 0;
    ForwardPresenceStatus status = ForwardPresenceStatus::Unsupported;
    std::string_view reason = "producer_state_unknown";
};
using ForwardFeatureSettings = std::array<std::optional<bool>, kForwardFeatureCount>;

// One completed frame/tick and an explicitly selected contributor ROI. Samples are
// last-contributor IDs at each forward-stage boundary, not final-colour guesses or
// all blended layers. Overlapping stages may count the same screen pixel twice.
// The draw join is O(1); storage is bounded to the registry's 65,536 draws (2 MiB).
// Construct a fresh collector per capture; caller owns immutable settings/table.
class DiagnosticForwardPresence {
public:
    DiagnosticForwardPresence(std::uint64_t frame,
                              std::uint64_t tick,
                              const DiagnosticExtent& extent,
                              const ForwardFeatureSettings& settings)
        : frame_(frame)
        , tick_(tick)
        , extent_(extent)
        , settings_(settings)
        , draws_(valid_extent(extent) ? std::make_unique<Draw[]>(kMaxDraws) : nullptr) {
        if (!valid_extent(extent_)) {
            fail("invalid_contributor_extent");
        }
    }

    // Called only after the original instrumented submission obtained its ID.
    void submit(const DiagnosticSubmission& submission, const DiagnosticId& id) {
        if (!failure_.empty()) {
            return;
        }
        const auto feature = classify(submission.shader_variant);
        if (!feature.has_value()) {
            return;
        }
        if (*feature == ForwardFeature::Waterfall && submission.generated_mesh.ends_with("/pool")) {
            return; // A visible plunge pool is not a visible waterfall sheet.
        }
        if (id[0] == 0 || id[0] > kMaxDraws || id[0] <= last_draw_ ||
            submission.pass != DiagnosticPass::Forward || !ValidDiagnosticId(id) ||
            id[2] != submission.lod ||
            id[3] != (static_cast<std::uint32_t>(submission.ownership) |
                      (static_cast<std::uint32_t>(submission.pass) << 8)) ||
            submission.provenance == DiagnosticProvenance::Unsupported) {
            fail("invalid_contributor_submission");
            return;
        }
        last_draw_ = id[0];
        draws_[id[0] - 1] = {.id = id, .feature = *feature, .visible = false};
        ++rows_[index(*feature)].submitted_draws;
    }

    // A bounded complete plane, sampled before the next forward stage overwrites
    // it. Exact metadata joins prevent stale/malformed IDs from becoming evidence.
    // Validate first, then update counters; refusal leaves previous counts intact.
    bool join(const DiagnosticDrawTable& table,
              std::span<const DiagnosticId> plane,
              const DiagnosticExtent& extent,
              std::uint64_t frame,
              std::uint64_t tick) {
        if (!failure_.empty()) {
            return false;
        }
        if (frame != frame_ || tick != tick_ || extent.width != extent_.width ||
            extent.height != extent_.height || !valid_extent(extent) ||
            plane.size() != static_cast<std::size_t>(extent.width) * extent.height ||
            stages_ == 64 || !table.complete_coverage()) {
            fail("contributor_frame_or_plane_unavailable");
            return false;
        }
        for (const auto& id : plane) {
            if (id == DiagnosticId{}) {
                continue;
            }
            const auto* resolved = table.resolve(id[0]);
            if (!resolved || *resolved != id || id[0] > kMaxDraws) {
                fail("unresolved_contributor_identity");
                return false;
            }
            const auto& draw = draws_[id[0] - 1];
            if (draw.id[0] != 0 && draw.id != id) {
                fail("mismatched_contributor_identity");
                return false;
            }
        }
        for (const auto& id : plane) {
            if (id[0] == 0 || id[0] <= joined_draw_) {
                continue;
            }
            auto& draw = draws_[id[0] - 1];
            if (draw.id[0] == 0) {
                continue; // Other registered forward producers are not these features.
            }
            auto& row = rows_[index(draw.feature)];
            ++row.contributor_samples;
            if (!draw.visible) {
                draw.visible = true;
                ++row.visible_draws;
            }
        }
        joined_draw_ = last_draw_;
        ++stages_;
        return true;
    }

    // The capture scope owns the frame/tick binding; adapters using external
    // buffers instead call join() with their observed frame/tick explicitly.
    bool join_stage(const DiagnosticDrawTable& table,
                    std::span<const DiagnosticId> plane,
                    const DiagnosticExtent& extent) {
        return join(table, plane, extent, frame_, tick_);
    }

    void unavailable() noexcept {
        fail("diagnostic_capture_incomplete");
    }

    bool can_sample() const {
        return failure_.empty() && stages_ < 64;
    }

    // Capture construction refuses reused/failed evidence rather than retaining
    // a previous frame's positive counters when the new frame submits nothing.
    bool fresh() const {
        return failure_.empty() && stages_ == 0 && last_draw_ == 0;
    }

    std::array<ForwardFeatureEvidence, kForwardFeatureCount> evidence() const {
        auto result = rows_;
        for (std::size_t feature = 0; feature < result.size(); ++feature) {
            auto& row = result[feature];
            if (!failure_.empty()) {
                row.reason = failure_;
            } else if (settings_[feature] == false && row.submitted_draws != 0) {
                row.reason = "disabled_producer_submitted_draws";
            } else if (settings_[feature] == false) {
                row.status = ForwardPresenceStatus::Disabled;
                row.reason = "feature_disabled";
            } else if (feature == index(ForwardFeature::River)) {
                row.reason = "water_surface_has_no_resolved_river_site";
            } else if (feature == index(ForwardFeature::Spray)) {
                row.reason = "particle_batch_has_no_spray_contributor_identity";
            } else if (!settings_[feature].has_value()) {
                row.reason = "producer_state_unknown";
            } else if (joined_draw_ != last_draw_) {
                row.reason = "contributor_stage_not_sampled";
            } else if (row.contributor_samples != 0) {
                row.status = ForwardPresenceStatus::Present;
                row.reason = "instrumented_contributor_samples";
            } else if (row.submitted_draws != 0) {
                row.status = ForwardPresenceStatus::Occluded;
                row.reason = "submitted_without_visible_contribution";
            } else {
                row.status = ForwardPresenceStatus::Absent;
                row.reason = "no_submitted_draws";
            }
        }
        return result;
    }

    static std::optional<ForwardFeature> classify(std::string_view producer) {
        if (producer == "water")
            return ForwardFeature::WaterSurface;
        if (producer == "waterfall")
            return ForwardFeature::Waterfall;
        if (producer == "magical_particles")
            return ForwardFeature::Particles;
        if (producer == "foliage_gpu" || producer == "foliage_cpu" ||
            producer == "foliage_qualification_prefix" ||
            producer == "foliage_qualification_sample" ||
            producer == "foliage_qualification_suffix")
            return ForwardFeature::Foliage;
        return std::nullopt;
    }

private:
    void fail(std::string_view reason) noexcept {
        failure_ = reason; // Only internal static labels reach this storage.
    }
    static constexpr std::uint32_t kMaxDraws = 65536;
    struct Draw {
        DiagnosticId id{};
        ForwardFeature feature = ForwardFeature::WaterSurface;
        bool visible = false;
    };
    static constexpr std::size_t index(ForwardFeature feature) {
        return static_cast<std::size_t>(feature);
    }
    static bool valid_extent(const DiagnosticExtent& extent) {
        return extent.width != 0 && extent.height != 0 && extent.width <= 16384 &&
               extent.height <= 16384 && extent.width <= 16777216 / extent.height;
    }
    std::uint64_t frame_;
    std::uint64_t tick_;
    DiagnosticExtent extent_;
    ForwardFeatureSettings settings_;
    std::unique_ptr<Draw[]> draws_;
    std::array<ForwardFeatureEvidence, kForwardFeatureCount> rows_{};
    std::uint32_t last_draw_ = 0;
    std::uint32_t joined_draw_ = 0;
    std::uint32_t stages_ = 0;
    std::string_view failure_;
};

} // namespace Luminumbra::Rendering
