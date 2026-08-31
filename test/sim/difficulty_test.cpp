// game.difficulty: a PURE difficulty profile mapping ONE 0..1 scalar to a
// bundle of per-system tuning multipliers (plant growth, evolution mutation, disease
// virulence, fire dryness, predator speed, forage richness). These tests pin the
// rubric: difficulty01=0 yields the RELAXED band, =1 the HARSH band; each parameter
// is monotonic in difficulty01 (harder => more virulent/dry/fast predators, less
// growth/food); outputs stay within sane clamped ranges; the named presets land in
// the expected bands; and the mapping is a pure function (run==replay). No entt, no
// rng, no wall-clock — DifficultyAt operates on one float.
#include <gtest/gtest.h>

#include "game/DifficultyProfile.h"

namespace {

using luminumbra::game::DifficultyParams;
using luminumbra::game::DifficultyAt;
using luminumbra::game::PeacefulParams;
using luminumbra::game::NormalParams;
using luminumbra::game::HarshParams;
using luminumbra::game::kPeaceful01;
using luminumbra::game::kNormal01;
using luminumbra::game::kHarsh01;
using luminumbra::game::kRelaxGrowth;
using luminumbra::game::kHarshGrowth;
using luminumbra::game::kRelaxMutation;
using luminumbra::game::kHarshMutation;
using luminumbra::game::kRelaxDisease;
using luminumbra::game::kHarshDisease;
using luminumbra::game::kRelaxFire;
using luminumbra::game::kHarshFire;
using luminumbra::game::kRelaxPredator;
using luminumbra::game::kHarshPredator;
using luminumbra::game::kRelaxForage;
using luminumbra::game::kHarshForage;

// ---- endpoints hit the authored relaxed / harsh bands exactly ----

// At difficulty01 = 0 every parameter equals its RELAXED band endpoint: fast growth,
// gentle mutation, mild disease, damp fire, slow predators, rich forage.
TEST(Difficulty, ZeroIsRelaxedBand) {
    const DifficultyParams p = DifficultyAt(0.0f);
    EXPECT_FLOAT_EQ(p.growth_speed,      kRelaxGrowth);
    EXPECT_FLOAT_EQ(p.mutation_rate,     kRelaxMutation);
    EXPECT_FLOAT_EQ(p.disease_virulence, kRelaxDisease);
    EXPECT_FLOAT_EQ(p.fire_dryness,      kRelaxFire);
    EXPECT_FLOAT_EQ(p.predator_speed,    kRelaxPredator);
    EXPECT_FLOAT_EQ(p.forage_richness,   kRelaxForage);
}

// At difficulty01 = 1 every parameter equals its HARSH band endpoint: slow growth,
// volatile mutation, virulent disease, dry fire, fast predators, scarce forage.
TEST(Difficulty, OneIsHarshBand) {
    const DifficultyParams p = DifficultyAt(1.0f);
    EXPECT_FLOAT_EQ(p.growth_speed,      kHarshGrowth);
    EXPECT_FLOAT_EQ(p.mutation_rate,     kHarshMutation);
    EXPECT_FLOAT_EQ(p.disease_virulence, kHarshDisease);
    EXPECT_FLOAT_EQ(p.fire_dryness,      kHarshFire);
    EXPECT_FLOAT_EQ(p.predator_speed,    kHarshPredator);
    EXPECT_FLOAT_EQ(p.forage_richness,   kHarshForage);
}

// ---- rubric direction: harder really IS harder ----

// Harsh end vs relaxed end: the "danger" knobs go UP and the "comfort" knobs go DOWN
// as difficulty rises. This is the load-bearing meaning of the difficulty axis.
TEST(Difficulty, HarshIsHarderThanRelaxed) {
    const DifficultyParams lo = DifficultyAt(0.0f);
    const DifficultyParams hi = DifficultyAt(1.0f);
    // Danger increases.
    EXPECT_GT(hi.mutation_rate,     lo.mutation_rate);
    EXPECT_GT(hi.disease_virulence, lo.disease_virulence);
    EXPECT_GT(hi.fire_dryness,      lo.fire_dryness);
    EXPECT_GT(hi.predator_speed,    lo.predator_speed);
    // Comfort decreases.
    EXPECT_LT(hi.growth_speed,    lo.growth_speed);
    EXPECT_LT(hi.forage_richness, lo.forage_richness);
}

// ---- monotonicity across the full sweep ----

// Each parameter is monotonic in difficulty01 across a fine sweep of [0,1]: the
// danger knobs are non-decreasing, the comfort knobs non-increasing, step to step.
// Smoothstep is strictly monotonic on (0,1), so adjacent samples never reverse.
TEST(Difficulty, EachParamIsMonotonic) {
    const int N = 64;
    DifficultyParams prev = DifficultyAt(0.0f);
    for (int i = 1; i <= N; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(N);
        const DifficultyParams cur = DifficultyAt(t);
        // Danger knobs: non-decreasing.
        EXPECT_GE(cur.mutation_rate,     prev.mutation_rate)     << "i=" << i;
        EXPECT_GE(cur.disease_virulence, prev.disease_virulence) << "i=" << i;
        EXPECT_GE(cur.fire_dryness,      prev.fire_dryness)      << "i=" << i;
        EXPECT_GE(cur.predator_speed,    prev.predator_speed)    << "i=" << i;
        // Comfort knobs: non-increasing.
        EXPECT_LE(cur.growth_speed,    prev.growth_speed)    << "i=" << i;
        EXPECT_LE(cur.forage_richness, prev.forage_richness) << "i=" << i;
        prev = cur;
    }
}

// ---- clamped / sane ranges, including out-of-band inputs ----

// Out-of-band inputs saturate to the endpoints rather than extrapolating: a negative
// difficulty reads as fully relaxed, a >1 difficulty as fully harsh.
TEST(Difficulty, OutOfBandInputsSaturate) {
    const DifficultyParams below = DifficultyAt(-5.0f);
    const DifficultyParams above = DifficultyAt(3.0f);
    const DifficultyParams relaxed = DifficultyAt(0.0f);
    const DifficultyParams harsh   = DifficultyAt(1.0f);
    EXPECT_FLOAT_EQ(below.growth_speed,    relaxed.growth_speed);
    EXPECT_FLOAT_EQ(below.disease_virulence, relaxed.disease_virulence);
    EXPECT_FLOAT_EQ(above.growth_speed,    harsh.growth_speed);
    EXPECT_FLOAT_EQ(above.disease_virulence, harsh.disease_virulence);
}

// Every multiplier stays within a sane, strictly-positive band across the whole
// sweep — no system is ever handed a 0x, negative, or absurd multiplier.
TEST(Difficulty, MultipliersStayInSaneBands) {
    const int N = 64;
    for (int i = 0; i <= N; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(N);
        const DifficultyParams p = DifficultyAt(t);
        for (float v : {p.growth_speed, p.mutation_rate, p.disease_virulence,
                        p.fire_dryness, p.predator_speed, p.forage_richness}) {
            EXPECT_GT(v, 0.0f) << "t=" << t; // strictly positive
            EXPECT_LE(v, 3.0f) << "t=" << t; // never absurd
        }
    }
}

// ---- named presets land in the expected bands ----

// Peaceful == fully relaxed, Harsh == fully harsh, and Normal sits strictly between
// the two on every parameter (the midpoint of the sweep).
TEST(Difficulty, PresetsMatchExpectedBands) {
    const DifficultyParams peaceful = PeacefulParams();
    const DifficultyParams normal   = NormalParams();
    const DifficultyParams harsh    = HarshParams();

    // Presets equal DifficultyAt at their anchor points.
    EXPECT_FLOAT_EQ(peaceful.disease_virulence, DifficultyAt(kPeaceful01).disease_virulence);
    EXPECT_FLOAT_EQ(harsh.disease_virulence,    DifficultyAt(kHarsh01).disease_virulence);

    // Peaceful is the relaxed band; Harsh is the harsh band.
    EXPECT_FLOAT_EQ(peaceful.disease_virulence, kRelaxDisease);
    EXPECT_FLOAT_EQ(harsh.disease_virulence,    kHarshDisease);

    // Normal sits strictly between on a danger knob and a comfort knob.
    EXPECT_GT(normal.disease_virulence, peaceful.disease_virulence);
    EXPECT_LT(normal.disease_virulence, harsh.disease_virulence);
    EXPECT_LT(normal.growth_speed, peaceful.growth_speed);
    EXPECT_GT(normal.growth_speed, harsh.growth_speed);
    EXPECT_FLOAT_EQ(kNormal01, 0.5f);
}

// ---- determinism: run == replay ----

// The same difficulty01 yields a BIT-IDENTICAL DifficultyParams on every call (pure,
// libm-free, no rng/wall-clock). Exact == equality, not tolerance.
TEST(Difficulty, RunEqualsReplay) {
    const float probes[] = {0.0f, 0.13f, 0.5f, 0.777f, 1.0f};
    for (float d : probes) {
        const DifficultyParams a = DifficultyAt(d);
        const DifficultyParams b = DifficultyAt(d);
        EXPECT_EQ(a.growth_speed,      b.growth_speed);
        EXPECT_EQ(a.mutation_rate,     b.mutation_rate);
        EXPECT_EQ(a.disease_virulence, b.disease_virulence);
        EXPECT_EQ(a.fire_dryness,      b.fire_dryness);
        EXPECT_EQ(a.predator_speed,    b.predator_speed);
        EXPECT_EQ(a.forage_richness,   b.forage_richness);
    }
}

} // namespace
