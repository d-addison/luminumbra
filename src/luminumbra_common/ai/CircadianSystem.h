#pragma once

// sim.day_night_activity: DIURNAL / NOCTURNAL ACTIVITY.
//
// Creatures keep a daily rhythm: a DIURNAL creature is most active at NOON and at rest at
// MIDNIGHT; a NOCTURNAL creature is the inverse. This system reads the day clock's
// time-of-day in [0,1] (0 = midnight, 0.5 = noon, 1 = midnight again) and writes each
// participating creature's ACTIVITY in [0,1] — a smooth, continuous, libm-free day curve.
// CreatureBrainSystem consumes activity to choose sleep and wake behaviour;
// this system maintains the scalar.
//
// THE CURVE. activity is built from the IEEE-deterministic cosine wrapper
// (DeterministicMath::Cos) so it is bit-stable and has NO discontinuity across the day
// boundary (cos is periodic, so t=0 and t=1 land on the same value):
//   diurnal  a(t) = 0.5 * (1 - cos(2*pi*t))   -> 0 at midnight, 1 at noon, 0 at midnight
//   nocturnal a(t) = 0.5 * (1 + cos(2*pi*t))  -> 1 at midnight, 0 at noon, 1 at midnight
// Both stay within [0,1] analytically; we still clamp to absorb the polynomial's ~1e-6
// approximation error so the written value is never a hair outside the range.
//
// DETERMINISM. id-ordered traversal (sort by entt::to_integral) so the (independent)
// per-creature writes happen in a fixed order. Math is DeterministicMath only (Cos; +-*/,
// clamped to [0,1]); time_of_day01 is a plain float the caller supplies from the day clock
// (NOT wall-clock). NO rng is consumed (the curve is a pure function of the time input), NO
// libm transcendentals. The same roster + the same time_of_day01 always produce the same
// activity (run==replay). Seed offset +29 is reserved for collision-avoidance only and never
// drawn.
//
// GATING. A creature only participates if it carries a CircadianComponent (the opt-in). A
// world whose creatures carry none — and any world with no creatures — does nothing, so the
// canonical NetworkStateHash baseline stays byte-identical.

#include <algorithm>
#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

#include "../components/CircadianComponents.h"
#include "../core/DeterministicMath.h"

namespace luminumbra::ai {

namespace Comp = ::Luminumbra::Components;
namespace dm = ::Luminumbra::DeterministicMath;

// Reserved seed-stream offset (registry: ... photo+24, herd-alarm+26, day-night+29). This
// system is deterministic and consumes NO rng; the offset is recorded for collision-avoidance
// only.
inline constexpr std::uint64_t kCircadianSeedOffset = 29ull;

struct CircadianStats {
    int participants = 0;  // creatures carrying a CircadianComponent that took part
    int diurnal = 0;       // of those, how many are diurnal (nocturnal == 0)
    int nocturnal = 0;     // of those, how many are nocturnal (nocturnal != 0)
};

[[nodiscard]] inline float CircadianClamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

// CircadianActivity: the pure day curve for one creature. `time_of_day01` is wrapped into
// [0,1) first (the caller's clock may overshoot at a day boundary), then mapped to an angle
// and run through DeterministicMath::Cos. Returns activity in [0,1]: diurnal peaks at noon
// (t=0.5), nocturnal peaks at midnight (t=0). Pure + deterministic; no rng, no libm.
[[nodiscard]] inline float CircadianActivity(float time_of_day01, bool nocturnal,
                                             float amplitude = 1.0f) {
    // Wrap to [0,1) so a clock that reports e.g. 1.25 or -0.1 still maps onto the day. Done
    // with float +-* and an integer floor so it is bit-stable (no libm fmod).
    float t = time_of_day01;
    const float floor_t = static_cast<float>(static_cast<std::int64_t>(t)) -
                          (t < 0.0f ? 1.0f : 0.0f);
    t = t - floor_t;            // now t in [0,1) (the -1 above handles negatives).
    if (t >= 1.0f) t -= 1.0f;   // guard the exact-integer edge.
    if (t < 0.0f) t = 0.0f;     // numerical safety.

    // Angle for one full day. cos(2*pi*t): +1 at t=0 (midnight), -1 at t=0.5 (noon).
    const float c = dm::Cos(dm::kTwoPi * t);

    // Diurnal: 0.5*(1 - cos) -> 0 at midnight, 1 at noon.
    // Nocturnal: 0.5*(1 + cos) -> 1 at midnight, 0 at noon (the inverse).
    // amplitude scales day/night contrast (1.0 = default curve, byte-identical; >1 sharpens,
    // <1 flattens). Clamp keeps activity in [0,1].
    const float a = amplitude * (nocturnal ? 0.5f * (1.0f + c) : 0.5f * (1.0f - c));
    return CircadianClamp01(a);
}

// RunCircadianOnTick: write every participating creature's activity from the day clock.
// id-ordered. `time_of_day01` is the caller-supplied fraction of the day in [0,1] (0/1 =
// midnight, 0.5 = noon). Returns participation stats. Empty roster -> pure no-op.
inline CircadianStats RunCircadianOnTick(entt::registry& reg, float time_of_day01,
                                         float amplitude = 1.0f) {
    CircadianStats stats;

    auto view = reg.view<Comp::CircadianComponent>();

    // id-ordered participant list (deterministic traversal). The per-creature writes are
    // independent (each reads only its own component + the shared time input), so ordering
    // cannot change the outcome, but we sort to keep traversal canonical.
    std::vector<entt::entity> ents(view.begin(), view.end());
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });
    if (ents.empty()) return stats;  // empty roster -> pure no-op.

    for (auto e : ents) {
        auto& cc = view.get<Comp::CircadianComponent>(e);
        const bool nocturnal = cc.nocturnal != 0;
        cc.activity = CircadianActivity(time_of_day01, nocturnal, amplitude);
        ++stats.participants;
        if (nocturnal) {
            ++stats.nocturnal;
        } else {
            ++stats.diurnal;
        }
    }

    return stats;
}

}  // namespace luminumbra::ai
