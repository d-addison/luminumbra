#include "luminumbra_common/world/FarHorizon.h"
#include "luminumbra_common/world/FarTierTable.h"

#include <gtest/gtest.h>

namespace {

using namespace Luminumbra::World;

TEST(FarHorizon, PlaneEqualsOuterRadiusPlusMargin) {
    EXPECT_EQ(kFarPlaneMeters, static_cast<float>(kFarOuterRadiusMeters + kFarPlaneMarginMeters));
    EXPECT_EQ(kFarPlaneMeters, 17408.0f);
}

TEST(FarHorizon, LegacyPlaneIsUnchanged) {
    EXPECT_EQ(kLegacyFarPlaneMeters, 3200.0f);
}

TEST(FarHorizon, LadderPlaneClearsEveryTierRadius) {
    for (const auto& tier : kFarTierTable) {
        SCOPED_TRACE(tier.outer_radius_meters);
        EXPECT_GT(kFarPlaneMeters, static_cast<float>(tier.outer_radius_meters));
    }
}

} // namespace
