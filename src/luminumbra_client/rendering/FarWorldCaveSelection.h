#pragma once

#include "FarWorldSelection.h"
#include "luminumbra_common/world/FarVolume.h"

#include <optional>

namespace Luminumbra::Rendering {

// Recorded fallback until native measurements and design review select a policy.
// Neither camera observations nor pacing/residency limits participate in selection.
inline constexpr World::FarCaveMode kDefaultFarWorldCavePolicy = World::FarCaveMode::BandLimited;
inline constexpr float kDefaultFarWorldCaveFieldCode = 0.0f;

// Exact scalar codes only: no rounding, clamping, or invalid-code fallback.
inline std::optional<World::FarCaveMode> ResolveFarWorldCaveField(double code) noexcept {
    if (code == 0.0)
        return World::FarCaveMode::BandLimited;
    if (code == 1.0)
        return World::FarCaveMode::BoxFiltered;
    return std::nullopt;
}

// Value-only diagnostic seam. Invalid requested codes are absent; Legacy has no
// effective cave policy, even when an immutable Ladder fixture remains bound.
struct FarWorldCavePolicy {
    std::optional<World::FarCaveMode> requested = kDefaultFarWorldCavePolicy;
    std::optional<World::FarCaveMode> bound;
    std::optional<World::FarCaveMode> effective;
};

inline FarWorldCavePolicy SelectFarWorldCavePolicy(std::optional<World::FarCaveMode> requested,
                                                   std::optional<World::FarCaveMode> bound,
                                                   const FarWorldSelection& selection) noexcept {
    return {requested, bound, selection.effective == FarWorldMode::Ladder ? bound : std::nullopt};
}

} // namespace Luminumbra::Rendering
