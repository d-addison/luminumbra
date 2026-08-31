#pragma once

// sim.herd_alarm: COLLECTIVE VIGILANCE. The presence of this component is the
// per-entity opt-in for luminumbra::ai::RunHerdAlarmOnTick (mirrors the PlantTag /
// CreatureGenomeComponent gating pattern): a world whose creatures carry NO
// AlarmComponent — and any world with NO creatures — runs the herd-alarm system as a
// pure no-op, so the canonical headless NetworkStateHash baseline stays byte-identical.
//
// The alarm system maintains a propagating "alarm field" over the creatures that carry
// this component: an alarmed creature (alarmed==1, or level above a threshold) RAISES the
// alarm level of nearby SAME-ROLE neighbours (same CreatureComponent.is_predator), so a
// startle ripples through the herd in  over successive ticks (collective vigilance).
// Every creature's level DECAYS toward 0 each tick when no source is nearby. The
// brain/flee reaction that consumes this field is wired separately (this component is
// pure sim state — a clamped float level + a uint8 flag; geometry/rendering is elsewhere).
//
// DETERMINISM: level is a plain float kept clamped to [0,1] and updated only via
// DeterministicMath (Sqrt for distance; +-*/), so the same roster + inputs always
// produce the same field (run==replay). No rng is consumed (propagation is deterministic;
// the registry reserves seed offset +26 for collision-avoidance only).

#include <cstdint>

namespace Luminumbra::Components {

struct AlarmComponent {
    // Current alarm level, clamped [0,1]. 0 = calm; rises when alarmed or when an alarmed
    // same-role neighbour is in range; decays toward 0 each tick otherwise. The brain
    // reads this to decide whether to flee with the herd.
    float level = 0.0f;
    // Hard "I personally sensed a threat" flag (1 = alarmed). When set, the creature acts
    // as a FULL-strength alarm SOURCE this tick regardless of its accumulated level (its
    // effective source strength is clamped to at least 1.0). The perception/brain layer
    // sets this when a threat is sensed; the alarm system propagates from it and applies
    // decay to `level`. It does not itself clear this flag (the source owner owns it).
    std::uint8_t alarmed = 0;
};

} // namespace Luminumbra::Components
