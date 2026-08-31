#pragma once

// sim.predator_pack: EMERGENT PACK HUNTING. The presence of this component is the
// per-entity opt-in for luminumbra::ai::RunPredatorPackOnTick (mirrors the AlarmComponent /
// CreatureGenomeComponent gating pattern): a world whose creatures carry NO
// PackHunterComponent — and any world with NO creatures — runs the pack-hunting system as a
// pure no-op, so the canonical headless NetworkStateHash baseline stays byte-identical.
//
// When two or more predators (CreatureComponent.is_predator) that BOTH carry this component
// are within a pack radius of each other, they form a PACK. Instead of all charging the same
// point (which lets prey escape on the open flank), pack-mates each aim for a DIFFERENT
// approach angle spread around the shared nearest prey, so they ENCIRCLE it (a flanking
// pincer — emergent coordination). The pack-hunting system writes a per-predator coordination
// "wish" (coord_x / coord_z) — a unit-ish steering vector toward that predator's assigned
// flank approach point — which the orchestrator blends into the predator's movement. A lone
// predator (no pack-mate in range) gets in_pack=0 and a DIRECT vector straight at the prey.
//
// DETERMINISM: coord_x/coord_z are plain floats produced only via DeterministicMath
// (Cos/Sin for the flank angle, Sqrt for distance; +-*/), so the same roster + inputs always
// produce the same coordination field (run==replay). No rng is consumed (the flank assignment
// is a deterministic function of the id-sorted pack index); the registry reserves seed offset
// +31 for collision-avoidance only. The vector is pure sim state — geometry/rendering and the
// movement blend are owned elsewhere.

#include <cstdint>

namespace Luminumbra::Components {

struct PackHunterComponent {
    // Coordination wish: a steering vector (world XZ) the orchestrator blends into the
    // predator's movement. For a pack-mate it points at this predator's ASSIGNED flank
    // approach point around the shared prey (so the pack encircles); for a lone predator it
    // points straight at the prey. Both default to 0 (no wish) when no prey is in range.
    float coord_x = 0.0f;
    float coord_z = 0.0f;
    // 1 when this predator coordinated with at least one pack-mate in range this tick; 0 when
    // it hunted alone (no pack-mate within the pack radius). Telemetry / sub-hash cue.
    std::uint8_t in_pack = 0;
};

}  // namespace Luminumbra::Components
