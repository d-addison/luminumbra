#pragma once

// Test-support code only: never include from src/.

#include <gtest/gtest.h>

#include "luminumbra_common/fields/EnergyFieldState.h"
#include "luminumbra_common/world/ActiveRegionLedger.h"
#include "luminumbra_common/world/GameSession.h"

#include <array>
#include <cstdint>

namespace Luminumbra::TestSupport {

// Steps a session's energy field the way the session does. With active regions the
// pages need ledger owners and ages, so a bare field Tick would leave the next save
// refusing the mismatch; the origin region is pinned and stepped through the ledger.
inline void ApplySnapshotTestEnergy(world::GameSession& session, std::uint64_t tick) {
    auto* energy = session.GetEnergyFieldState();
    ASSERT_NE(energy, nullptr);
    if (!session.ActiveRegionsEnabled()) {
        energy->Tick(tick);
        return;
    }
    auto& ledger = const_cast<world::ActiveRegionLedger&>(session.GetActiveRegionLedger());
    const world::RegionKey owner{0, 0};
    ledger.pin(owner, true, session.GetWorldClock());
    luminumbra::fields::EnergyFieldState::RegionalResult result;
    ASSERT_TRUE(
        energy->TickRegional(tick, ledger, std::array{owner}, ledger.energy_ages(), result));
    ASSERT_TRUE(ledger.set_energy_ages(result.ages));
}

} // namespace Luminumbra::TestSupport
