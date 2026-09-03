#pragma once

// sim.territory: HOME-RANGE / TERRITORIALITY. A creature that carries a
// TerritoryComponent claims a HOME location (its spawn position) and prefers to stay near it:
// the territory system (luminumbra::ai::RunTerritoryOnTick) computes, each tick, a HOMING wish
// bias — a horizontal vector that points back toward home and GROWS the further the creature
// strays beyond its territory radius (zero while it is inside the radius). The orchestrator
// blends TerritoryBiasComponent.wish_{x,z} into the creature's movement so animals roam a
// bounded home range rather than wandering off forever, and so a predator that drifts into
// another predator's claimed territory is pushed back toward its own ground.
//
// GATING. Presence of TerritoryComponent is the per-entity OPT-IN. A world whose creatures
// carry no TerritoryComponent — and any world with no creatures — runs the system as a pure
// no-op, so the canonical headless NetworkStateHash baseline stays byte-identical.
//
// DETERMINISM. All fields are plain sim state (floats / a flag). The system reads/writes them
// with DeterministicMath only (Sqrt for distance; +-*/), id-ordered, no rng, no wall-clock —
// so run==replay holds. Geometry/rendering is separate; these are sim values only.

#include <cstdint>

namespace Luminumbra::Components {

// The creature's claimed home range. On the first territory tick (while established==0) the
// system stamps home_{x,z} from the creature's current TransformComponent position and sets
// established=1; thereafter home stays fixed and defines the centre of the home range.
struct TerritoryComponent {
    float home_x = 0.0f;          // world-X of the claimed home (valid once established==1)
    float home_z = 0.0f;          // world-Z of the claimed home (valid once established==1)
    float radius = 20.0f;         // home-range radius (world units); homing is 0 inside this
    std::uint8_t established = 0; // 0 = home not yet claimed; 1 = claimed (home_{x,z} valid)
};

// Output of the territory system: the per-tick HOMING wish bias the orchestrator blends into
// movement. It is a vector pointing from the creature TOWARD home, with magnitude rising with
// how far the creature has strayed PAST its radius (zero while inside the territory). The
// system creates/updates this component on every participating creature; defaults are a no-op
// (zero bias) so a creature whose system has not yet run does not perturb movement.
struct TerritoryBiasComponent {
    float wish_x = 0.0f; // homing bias toward home, X component (m/s-scaled steer)
    float wish_z = 0.0f; // homing bias toward home, Z component (m/s-scaled steer)
};

} // namespace Luminumbra::Components
