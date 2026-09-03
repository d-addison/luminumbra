#pragma once

// sim.day_night_activity: DIURNAL / NOCTURNAL ACTIVITY. The presence of this
// component is the per-entity opt-in for luminumbra::ai::RunCircadianOnTick (mirrors the
// AlarmComponent / CreatureGenomeComponent gating pattern): a world whose creatures carry
// NO CircadianComponent — and any world with NO creatures — runs the circadian system as a
// pure no-op, so the canonical headless NetworkStateHash baseline stays byte-identical.
//
// A creature is either DIURNAL (nocturnal == 0; active by day, peaks at noon) or NOCTURNAL
// (nocturnal != 0; active by night, peaks at midnight). Each tick the system reads the day
// clock's time-of-day in [0,1] (0 = midnight, 0.5 = noon) and writes a smooth ACTIVITY in
// [0,1] that CreatureBrainSystem consumes for sleep and wake behaviour. This
// component is pure sim state: a phenotype flag and a clamped activity value;
// geometry/rendering is elsewhere.
//
// DETERMINISM: activity is a plain float kept clamped to [0,1] and updated only via
// DeterministicMath (Cos for the smooth day curve; +-*/), so the same roster + the same
// time-of-day always produce the same activity (run==replay). No rng is consumed (the curve
// is a pure function of the time input; the registry reserves seed offset +29 for
// collision-avoidance only).

#include <cstdint>

namespace Luminumbra::Components {

struct CircadianComponent {
    // Phenotype: 0 = DIURNAL (peaks at noon), nonzero = NOCTURNAL (peaks at midnight).
    std::uint8_t nocturnal = 0;
    // Current activity, clamped [0,1]. 0 = fully at rest (the creature's "off" phase of the
    // day), 1 = fully active (its peak). Written each tick from the day clock; the brain /
    // locomotion layer scales behaviour by it. Defaults to 1.0 so a creature that is never
    // ticked reads as fully active rather than asleep.
    float activity = 1.0f;
};

} // namespace Luminumbra::Components
