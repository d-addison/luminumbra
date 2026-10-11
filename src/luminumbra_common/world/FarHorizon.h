#pragma once

// Camera far-plane constants for the legacy heightfield path and the volumetric
// far ladder declared in FarTierTable.h. The ladder plane is declared here and
// tested; the runtime camera keeps the legacy plane until the ladder renders
// (see docs/distant-world.md, Depth, fog, shadows, horizon).

#include "luminumbra_common/world/FarTierTable.h"

#include <cstdint>

namespace Luminumbra::World {

// Legacy two-tier heightfield path: 3,200 m.
inline constexpr float kLegacyFarPlaneMeters = 3200.0f;

// Margin between the ladder outer radius and its far plane.
inline constexpr std::uint32_t kFarPlaneMarginMeters = 1024u;

// Ladder far plane: outer radius plus margin (17,408 m).
inline constexpr float kFarPlaneMeters =
    static_cast<float>(kFarOuterRadiusMeters + kFarPlaneMarginMeters);

static_assert(kFarPlaneMeters == 17408.0f);
static_assert(kFarPlaneMeters > static_cast<float>(kFarOuterRadiusMeters));
static_assert(kLegacyFarPlaneMeters < kFarPlaneMeters);

} // namespace Luminumbra::World
