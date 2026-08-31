// game.photo_filters: a PURE, DETERMINISTIC post-capture FILTER/grade scorer
// (photography: how well a chosen colour grade SUITS a shot). These tests pin the
// rubric: Noir suits low-light/high-contrast scenes (beating Vivid there); Warm and
// Cool are mirror images keyed on scene warmth; Vivid rewards saturated subjects in
// good light; BestFilter returns the max-total filter; every score stays in [0,1];
// and the scorer is a pure function (same input -> bit-identical score, run==replay).
// No entt, no rng, no wall-clock — ScoreFilter operates on plain value structs.
#include <gtest/gtest.h>

#include "game/PhotoFilters.h"

namespace {

using luminumbra::game::BestFilter;
using luminumbra::game::FilterContext;
using luminumbra::game::FilterScore;
using luminumbra::game::kPhotoFilterCount;
using luminumbra::game::PhotoFilter;
using luminumbra::game::ScoreFilter;

// A low-light, high-contrast scene with little usable colour — classic noir setup.
FilterContext noirScene() {
    FilterContext c;
    c.scene_warmth01 = 0.45f;
    c.scene_contrast01 = 0.9f;
    c.subject_saturation01 = 0.25f;
    c.low_light01 = 0.9f;
    return c;
}

// A warm, golden-hour scene: high warmth, bright, moderate contrast + colour.
FilterContext warmScene() {
    FilterContext c;
    c.scene_warmth01 = 0.92f;
    c.scene_contrast01 = 0.5f;
    c.subject_saturation01 = 0.6f;
    c.low_light01 = 0.2f;
    return c;
}

// A cold, blue scene: low warmth, bright, moderate contrast.
FilterContext coolScene() {
    FilterContext c;
    c.scene_warmth01 = 0.08f;
    c.scene_contrast01 = 0.5f;
    c.subject_saturation01 = 0.5f;
    c.low_light01 = 0.2f;
    return c;
}

// A bright scene with a richly saturated subject — made for Vivid.
FilterContext vividScene() {
    FilterContext c;
    c.scene_warmth01 = 0.5f;
    c.scene_contrast01 = 0.5f;
    c.subject_saturation01 = 0.95f;
    c.low_light01 = 0.05f;
    return c;
}

// ---- NOIR ----

// On a low-light, high-contrast scene Noir scores higher (total) than Vivid: the
// dark frame is exactly noir's wheelhouse and exactly where vivid colour fails.
TEST(PhotoFilters, NoirBeatsVividOnLowLightHighContrast) {
    const FilterContext c = noirScene();
    const float noir = ScoreFilter(PhotoFilter::Noir, c).total;
    const float vivid = ScoreFilter(PhotoFilter::Vivid, c).total;
    EXPECT_GT(noir, vivid);
}

// Noir's suitability rises with darkness + contrast: the noir scene scores higher
// than a bright, flat one.
TEST(PhotoFilters, NoirSuitabilityTracksDarknessAndContrast) {
    FilterContext bright;
    bright.scene_contrast01 = 0.1f;
    bright.low_light01 = 0.1f;
    const float dark_total = ScoreFilter(PhotoFilter::Noir, noirScene()).total;
    const float lit_total = ScoreFilter(PhotoFilter::Noir, bright).total;
    EXPECT_GT(dark_total, lit_total);
}

// ---- WARM vs COOL (mirror images) ----

// On a warm/golden scene, the Warm grade beats the Cool grade.
TEST(PhotoFilters, WarmBeatsCoolOnWarmScene) {
    const FilterContext c = warmScene();
    EXPECT_GT(ScoreFilter(PhotoFilter::Warm, c).total, ScoreFilter(PhotoFilter::Cool, c).total);
}

// On a cold/blue scene, the Cool grade beats the Warm grade (the mirror).
TEST(PhotoFilters, CoolBeatsWarmOnCoolScene) {
    const FilterContext c = coolScene();
    EXPECT_GT(ScoreFilter(PhotoFilter::Cool, c).total, ScoreFilter(PhotoFilter::Warm, c).total);
}

// ---- VIVID ----

// Vivid rewards a saturated subject in good light: it scores higher on a bright,
// richly-coloured scene than on an identical scene that is dark.
TEST(PhotoFilters, VividRewardsSaturatedSubjectsInGoodLight) {
    const FilterContext good = vividScene();
    FilterContext dark = vividScene();
    dark.low_light01 = 0.95f; // same saturation, but now in the dark
    EXPECT_GT(ScoreFilter(PhotoFilter::Vivid, good).total,
              ScoreFilter(PhotoFilter::Vivid, dark).total);
}

// On the bright saturated scene, Vivid is the strongest stylized choice — it beats
// Noir there (the inverse of the noir-scene case).
TEST(PhotoFilters, VividBeatsNoirOnBrightSaturatedScene) {
    const FilterContext c = vividScene();
    EXPECT_GT(ScoreFilter(PhotoFilter::Vivid, c).total, ScoreFilter(PhotoFilter::Noir, c).total);
}

// ---- BestFilter ----

// BestFilter returns the filter with the maximum total: verify it matches an
// independent brute-force scan over every filter for several distinct scenes.
TEST(PhotoFilters, BestFilterReturnsMaxTotal) {
    const FilterContext scenes[] = {
        noirScene(),
        warmScene(),
        coolScene(),
        vividScene(),
    };
    for (const auto& c : scenes) {
        const PhotoFilter best = BestFilter(c);
        const float best_total = ScoreFilter(best, c).total;
        for (int i = 0; i < kPhotoFilterCount; ++i) {
            const PhotoFilter f = static_cast<PhotoFilter>(i);
            EXPECT_GE(best_total, ScoreFilter(f, c).total);
        }
    }
}

// BestFilter picks the expected look for clearly-characterized scenes.
TEST(PhotoFilters, BestFilterPicksExpectedLook) {
    EXPECT_EQ(BestFilter(noirScene()), PhotoFilter::Noir);
    EXPECT_EQ(BestFilter(warmScene()), PhotoFilter::Warm);
    EXPECT_EQ(BestFilter(coolScene()), PhotoFilter::Cool);
}

// ---- bounds ----

// Every field of every filter's score stays within [0,1] across a strong, neutral,
// and deliberately out-of-range FilterContext.
TEST(PhotoFilters, ScoresAreClampedToUnitRange) {
    FilterContext over; // out-of-range on purpose: must still clamp
    over.scene_warmth01 = 2.0f;
    over.scene_contrast01 = -1.0f;
    over.subject_saturation01 = 5.0f;
    over.low_light01 = -3.0f;

    FilterContext neutral; // all defaults (0.5)

    const FilterContext scenes[] = {noirScene(), warmScene(), neutral, over};
    for (const auto& c : scenes) {
        for (int i = 0; i < kPhotoFilterCount; ++i) {
            const FilterScore s = ScoreFilter(static_cast<PhotoFilter>(i), c);
            for (float v : {s.suitability, s.mood, s.total}) {
                EXPECT_GE(v, 0.0f);
                EXPECT_LE(v, 1.0f);
            }
        }
    }
}

// ---- determinism: run == replay ----

// The same FilterContext yields a BIT-IDENTICAL FilterScore on every evaluation
// (pure, libm-free, no rng/wall-clock). This is the run==replay contract.
TEST(PhotoFilters, RunEqualsReplay) {
    FilterContext c;
    c.scene_warmth01 = 0.37f;
    c.scene_contrast01 = 0.71f;
    c.subject_saturation01 = 0.58f;
    c.low_light01 = 0.64f;

    for (int i = 0; i < kPhotoFilterCount; ++i) {
        const PhotoFilter f = static_cast<PhotoFilter>(i);
        const FilterScore a = ScoreFilter(f, c);
        const FilterScore b = ScoreFilter(f, c);
        // Exact bit equality (==), not approximate: determinism, not tolerance.
        EXPECT_EQ(a.suitability, b.suitability);
        EXPECT_EQ(a.mood, b.mood);
        EXPECT_EQ(a.total, b.total);
    }
    EXPECT_EQ(BestFilter(c), BestFilter(c));
}

} // namespace
