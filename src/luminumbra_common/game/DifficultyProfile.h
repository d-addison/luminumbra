#pragma once

// game.difficulty: a PURE difficulty profile mapping ONE 0..1 scalar to a
// bundle of per-system tuning multipliers. This is the single knob a player (or a
// game-mode preset) turns to make the living world relaxed or harsh; the engine's
// individual sim systems (plant growth, evolution, disease, fire, predator AI,
// soil/food abundance) each read their own multiplier out of DifficultyParams and
// scale their tuning by it. Centralizing the mapping here keeps the "what does
// harder MEAN" rubric in one auditable, testable place instead of scattered magic
// numbers across every system.
//
// SCOPE. NO render, NO camera, NO GL, NO entt, NO rng. DifficultyAt is a pure
// function of one float; it returns a plain value struct. It does not touch any
// registry, component, or global, so it can never perturb a baseline hash. The
// orchestrator decides which systems consult it and how the multipliers fold into
// their existing tuning — this file only OWNS the curve.
//
// DETERMINISM CONTRACT (sim-adjacent: if a multiplier ever feeds world_hash it must
// be bit-stable). The mapping uses ONLY float +-*/ and clamping — no libm
// transcendentals (no exp/log/pow), no Luminumbra::DeterministicMath calls are even
// needed because the curves are smooth polynomials (smoothstep) which are pure
// float arithmetic. The same difficulty01 ALWAYS yields the same DifficultyParams
// (run==replay). No seed offset is consumed (pure mapping, none assigned).
//
// RUBRIC (monotonic in difficulty01 across [0,1]):
//   difficulty01 = 0.0  RELAXED / Peaceful
//     - growth_speed      HIGH  (plants grow fast, the grove fills in quickly)
//     - mutation_rate     LOW   (evolution drifts gently)
//     - disease_virulence LOW   (illness barely spreads)
//     - fire_dryness      LOW   (the world is damp; fire rarely ignites)
//     - predator_speed    LOW   (predators are slow and easy to outrun)
//     - forage_richness   HIGH  (soil + food abundant)
//   difficulty01 = 1.0  HARSH / Harsh
//     - growth_speed      LOW   (plants struggle, growth is slow)
//     - mutation_rate     HIGH  (evolution is volatile)
//     - disease_virulence HIGH  (illness rips through populations)
//     - fire_dryness      HIGH  (tinder-dry world, fire ignites easily)
//     - predator_speed    HIGH  (predators are fast and dangerous)
//     - forage_richness   LOW   (scarce soil + food)
//
// Each output is a SMOOTH (libm-free) curve of difficulty01, clamped to a sane band
// so no system is ever handed a 0x or absurd multiplier.

#include "GameMath.h" // luminumbra::game: Clamp01

namespace luminumbra::game {

// ---------------------------------------------------------------------------
// The tuning bundle. Every field is a unit-less multiplier a system applies to its
// own baseline tuning. 1.0 == "the engine's neutral default" for that knob; values
// above/below scale that knob up/down. The bands below keep every multiplier within
// a sane, non-degenerate range across the whole difficulty sweep.
// ---------------------------------------------------------------------------
struct DifficultyParams {
    float growth_speed = 1.0f;      // plant growth rate multiplier
    float mutation_rate = 1.0f;     // evolution mutation sigma multiplier
    float disease_virulence = 1.0f; // disease spread multiplier
    float fire_dryness = 1.0f;      // fire ignition-ease multiplier
    float predator_speed = 1.0f;    // predator move-speed multiplier
    float forage_richness = 1.0f;   // soil/food abundance multiplier
};

// ---------------------------------------------------------------------------
// Band endpoints. Each multiplier interpolates between its value at difficulty01=0
// (kRelax*) and difficulty01=1 (kHarsh*). Authored as the nearest float; all of the
// math below is float +-*/. The relaxed vs harsh ordering encodes the rubric's
// monotonic direction per knob (some increase with difficulty, some decrease).
// ---------------------------------------------------------------------------

// growth_speed: fast when relaxed, slow when harsh. DECREASING in difficulty.
inline constexpr float kRelaxGrowth = 1.60f;
inline constexpr float kHarshGrowth = 0.55f;

// mutation_rate: gentle when relaxed, volatile when harsh. INCREASING.
inline constexpr float kRelaxMutation = 0.50f;
inline constexpr float kHarshMutation = 2.00f;

// disease_virulence: barely spreads when relaxed, rampant when harsh. INCREASING.
inline constexpr float kRelaxDisease = 0.35f;
inline constexpr float kHarshDisease = 2.20f;

// fire_dryness: damp when relaxed, tinder-dry when harsh. INCREASING.
inline constexpr float kRelaxFire = 0.40f;
inline constexpr float kHarshFire = 2.10f;

// predator_speed: slow when relaxed, fast when harsh. INCREASING.
inline constexpr float kRelaxPredator = 0.70f;
inline constexpr float kHarshPredator = 1.55f;

// forage_richness: abundant when relaxed, scarce when harsh. DECREASING.
inline constexpr float kRelaxForage = 1.70f;
inline constexpr float kHarshForage = 0.50f;

// ---------------------------------------------------------------------------
// Pure float helpers (no libm).
// ---------------------------------------------------------------------------
// Hermite smoothstep s(t) = t*t*(3 - 2*t) on a clamped t in [0,1]. Pure float
// arithmetic (no libm). Monotonically increasing on [0,1] with zero slope at both
// ends, giving a smooth S-curve. s(0)=0, s(1)=1, s strictly increasing in between.
inline float SmoothStep01(float t) {
    const float u = Clamp01(t);
    return u * u * (3.0f - 2.0f * u);
}

// Smoothly interpolate from `a` (at t=0) to `b` (at t=1) using the smoothstep curve.
// Because SmoothStep01 is strictly increasing on (0,1), the result is strictly
// monotonic in t: increasing when b > a, decreasing when b < a. Endpoints are exact
// (a at t<=0, b at t>=1) so the relaxed/harsh bands are hit precisely.
inline float SmoothBand(float a, float b, float t) {
    const float s = SmoothStep01(t);
    return a + (b - a) * s;
}

// ---------------------------------------------------------------------------
// DifficultyAt. The public entry point: map one 0..1 scalar to the full tuning
// bundle. Pure + deterministic + monotonic per field; difficulty01 is clamped to
// [0,1] first so out-of-band inputs saturate to the relaxed/harsh endpoints rather
// than extrapolating to nonsense.
// ---------------------------------------------------------------------------
inline DifficultyParams DifficultyAt(float difficulty01) {
    const float t = Clamp01(difficulty01);
    DifficultyParams p;
    p.growth_speed = SmoothBand(kRelaxGrowth, kHarshGrowth, t);
    p.mutation_rate = SmoothBand(kRelaxMutation, kHarshMutation, t);
    p.disease_virulence = SmoothBand(kRelaxDisease, kHarshDisease, t);
    p.fire_dryness = SmoothBand(kRelaxFire, kHarshFire, t);
    p.predator_speed = SmoothBand(kRelaxPredator, kHarshPredator, t);
    p.forage_richness = SmoothBand(kRelaxForage, kHarshForage, t);
    return p;
}

// ---------------------------------------------------------------------------
// Named presets. These are the canonical difficulty01 anchor points; a game-mode
// menu can hand DifficultyAt one of these directly. Peaceful = fully relaxed,
// Normal = the midpoint, Harsh = fully harsh.
// ---------------------------------------------------------------------------
inline constexpr float kPeaceful01 = 0.0f;
inline constexpr float kNormal01 = 0.5f;
inline constexpr float kHarsh01 = 1.0f;

// The materialized parameter bundles for the named presets, for callers that want
// the tuning directly without re-deriving it. (DifficultyAt is constexpr-friendly
// in spirit but inline-only here; these are computed at startup via the function.)
inline DifficultyParams PeacefulParams() {
    return DifficultyAt(kPeaceful01);
}
inline DifficultyParams NormalParams() {
    return DifficultyAt(kNormal01);
}
inline DifficultyParams HarshParams() {
    return DifficultyAt(kHarsh01);
}

} // namespace luminumbra::game
