#pragma once

#include "RenderWorkload.h"
#include "luminumbra_client/rendering/FarWorldDiagnostics.h"
#include <span>

namespace Luminumbra::Client::Measurement {

struct FarBudgetTier {
    Rendering::FarWorldTierDiagnostic observation;
    Rendering::FarVolumeTierWork work;
    bool current_coverage = false;
};

struct FarBudgetObservation {
    std::uint64_t render_frame = 0, world_epoch = 0, target_serial = 0;
    std::uint64_t target_cohort = 0, content_identity = 0, bake_generation = 0;
    bool accepted = false, current_coverage = false;
    std::array<FarBudgetTier, World::kFarTierCount> tiers{};
};

// Capture-only projection of the bounded value snapshot, never of the world store.
FarBudgetObservation ProjectFarBudget(const Rendering::FarWorldDiagnosticSnapshot&,
                                      bool submission_allowed = true) noexcept;

enum class FarFillKind {
    Cold,
    Epoch,
    EditBake,
    Cohort,
    Refill
};
struct FarBudgetFrame {
    FarBudgetObservation observed;
    std::optional<std::uint64_t> timing_frame;
    double elapsed_ms = 0, render_wall_ms = 0, origin_ms = 0;
    std::optional<double> accepted_ms, completed_ms;
    std::array<std::optional<double>, World::kFarTierCount> tier_completed_ms{};
    std::size_t event = 0;
    FarFillKind kind = FarFillKind::Cold;
    bool timed_out = false;
};

class FarBudgetMeasurement {
public:
    // Admission reserves all phases within this fixed storage bound.
    // Exhaustion refuses evidence; no observation is overwritten.
    static constexpr std::size_t kMaxObservations = kMaxBudgetObservations;
    explicit FarBudgetMeasurement(std::size_t capacity)
        : frames_(checked_capacity(capacity)) {}

    bool record(const FarBudgetObservation& observed,
                double elapsed_ms,
                double render_wall_ms,
                std::optional<std::uint64_t> timing_frame = std::nullopt) noexcept {
        if (count_ == frames_.size() || observed.render_frame == 0 ||
            (count_ ? observed.render_frame - 1 != frames_[count_ - 1].observed.render_frame
                    : observed.render_frame != 1) ||
            !std::isfinite(elapsed_ms) || elapsed_ms < 0 || !std::isfinite(render_wall_ms) ||
            render_wall_ms < 0 || render_wall_ms > elapsed_ms ||
            (!count_ && std::abs(render_wall_ms - elapsed_ms) > 1e-6) ||
            (count_ && (elapsed_ms < frames_[count_ - 1].elapsed_ms ||
                        render_wall_ms > elapsed_ms - frames_[count_ - 1].elapsed_ms + 1e-6)))
            return false;
        FarBudgetFrame next;
        if (count_)
            next = frames_[count_ - 1];
        const bool epoch_changed =
            last_epoch_ != 0 && observed.world_epoch != 0 && observed.world_epoch != last_epoch_;
        const bool bake_changed = last_epoch_ != 0 && observed.world_epoch == last_epoch_ &&
                                  observed.bake_generation != last_generation_;
        const bool changed = epoch_changed || bake_changed ||
                             (bound_ && observed.accepted &&
                              (observed.target_serial != binding_.target_serial ||
                               observed.target_cohort != binding_.target_cohort ||
                               observed.content_identity != binding_.content_identity));
        bool covered = observed.accepted && observed.current_coverage;
        for (const auto& tier : observed.tiers)
            covered = covered && tier.current_coverage;
        if (changed || (next.completed_ms && !covered)) {
            const auto event = next.event + 1;
            const auto kind = !changed        ? FarFillKind::Refill
                              : epoch_changed ? FarFillKind::Epoch
                              : bake_changed  ? FarFillKind::EditBake
                                              : FarFillKind::Cohort;
            next = {};
            next.event = event;
            next.kind = kind;
            next.origin_ms = elapsed_ms - render_wall_ms;
            bound_ = false;
        }
        if (observed.accepted && !bound_) {
            binding_ = observed;
            bound_ = true;
            next.accepted_ms = elapsed_ms;
        }
        if (observed.world_epoch != 0) {
            last_epoch_ = observed.world_epoch;
            last_generation_ = observed.bake_generation;
        }
        next.observed = observed;
        next.timing_frame = timing_frame;
        next.elapsed_ms = elapsed_ms;
        next.render_wall_ms = render_wall_ms;
        // Same inclusive 90-second observation deadline as station readiness.
        next.timed_out =
            next.timed_out || (!next.completed_ms && elapsed_ms - next.origin_ms > 90000.0);
        if (!next.timed_out && observed.accepted) {
            for (std::size_t i = 0; i < observed.tiers.size(); ++i) {
                if (!observed.tiers[i].current_coverage)
                    next.tier_completed_ms[i].reset();
                else if (!next.tier_completed_ms[i])
                    next.tier_completed_ms[i] = elapsed_ms;
            }
            if (covered && !next.completed_ms)
                next.completed_ms = elapsed_ms;
        }
        frames_[count_++] = next;
        return true;
    }

    std::span<const FarBudgetFrame> frames() const noexcept {
        return {frames_.data(), count_};
    }

private:
    static std::size_t checked_capacity(std::size_t capacity) {
        if (!capacity || capacity > kMaxObservations ||
            capacity > std::numeric_limits<std::size_t>::max() / sizeof(FarBudgetFrame))
            throw std::invalid_argument("invalid budget observation capacity");
        return capacity;
    }
    std::vector<FarBudgetFrame> frames_;
    FarBudgetObservation binding_;
    std::size_t count_ = 0;
    std::uint64_t last_epoch_ = 0, last_generation_ = 0;
    bool bound_ = false;
};
} // namespace Luminumbra::Client::Measurement
