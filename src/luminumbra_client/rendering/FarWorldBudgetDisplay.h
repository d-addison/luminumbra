#pragma once

#include "FarWorldDiagnostics.h"

#include <array>
#include <cstddef>
#include <optional>

namespace Luminumbra::Rendering {

struct FarWorldBudgetDisplayTier {
    std::optional<FarWorldTierDiagnostic> diagnostic = std::nullopt;
    std::optional<FarVolumeTierWork> work = std::nullopt;
};
using FarWorldBudgetDisplay = std::array<FarWorldBudgetDisplayTier, World::kFarTierCount>;

// Owner-thread value projection: fixed V1-V5 storage, no allocation or live
// controller access. Keep CPU/GPU/total samples verbatim; concurrent sampling
// need not reconcile. A missing pool has limit 0; an unlimited pool reports
// UINT64_MAX. Missing captures/pools and frames without advancement are unknown,
// not measured zero. Incomplete tile capture does not invalidate pool samples.
inline FarWorldBudgetDisplay
ProjectFarWorldBudgetDisplay(const FarWorldDiagnosticSnapshot& snapshot) noexcept {
    FarWorldBudgetDisplay display{};
    if (!snapshot.captured)
        return display;
    for (std::size_t i = 0; i < display.size(); ++i) {
        if (snapshot.tiers[i].resident.memory.limit == 0)
            continue;
        display[i].diagnostic = snapshot.tiers[i];
        if (snapshot.advanced && snapshot.selection.effective == FarWorldMode::Ladder)
            display[i].work = snapshot.upload.tiers[i];
    }
    return display;
}

} // namespace Luminumbra::Rendering
