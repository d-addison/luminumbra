// game.photo_scoring: a PURE, DETERMINISTIC capture-composition scorer
// (photography de-risk: the photography game loop's core, no render/camera/GL). These
// tests pin the rubric: a rule-of-thirds shot beats a dead-center or edge-clipped
// one; a blown-out or too-dark frame loses LIGHTING; out-of-focus loses FOCUS; a
// rarer subject scores higher RARITY; an empty shot is a defined low score; and
// the scorer is a pure function (same input -> same score, run==replay). No entt,
// no rng, no wall-clock — ScorePhoto operates on plain value structs.
#include <gtest/gtest.h>

#include <vector>

#include "systems/PhotoScoring.h"

namespace {

using luminumbra::photo::kThird;
using luminumbra::photo::PhotoScore;
using luminumbra::photo::PhotoShot;
using luminumbra::photo::PhotoSubject;
using luminumbra::photo::ScorePhoto;

// Build a single-subject shot with sensible defaults (mid exposure, sharp focus).
PhotoShot oneSubject(float x,
                     float y,
                     float light = 0.7f,
                     float size = 0.3f,
                     int species = 1,
                     float exposure = 0.5f,
                     float focus = 1.0f) {
    PhotoShot shot;
    PhotoSubject s;
    s.ndc_x = x;
    s.ndc_y = y;
    s.light = light;
    s.size = size;
    s.species_id = species;
    shot.subjects.push_back(s);
    shot.exposure = exposure;
    shot.focus = focus;
    return shot;
}

// ---- gating / defined edge case ----

// An empty shot is a defined low score: every axis and the total are exactly 0.
TEST(PhotoScoring, EmptyShotIsDefinedLowScore) {
    PhotoShot empty; // no subjects
    const PhotoScore sc = ScorePhoto(empty);
    EXPECT_FLOAT_EQ(sc.composition, 0.0f);
    EXPECT_FLOAT_EQ(sc.lighting, 0.0f);
    EXPECT_FLOAT_EQ(sc.focus, 0.0f);
    EXPECT_FLOAT_EQ(sc.rarity, 0.0f);
    EXPECT_FLOAT_EQ(sc.total, 0.0f);
}

// ---- COMPOSITION ----

// A subject placed on a rule-of-thirds power point scores higher composition than
// the same subject dead-center.
TEST(PhotoScoring, ThirdsBeatsCenterComposition) {
    const PhotoShot thirds = oneSubject(kThird, kThird);
    const PhotoShot center = oneSubject(0.0f, 0.0f);
    const float a = ScorePhoto(thirds).composition;
    const float b = ScorePhoto(center).composition;
    EXPECT_GT(a, b);
}

// A subject placed on a thirds point scores higher composition than the same
// subject clipped hard against the frame edge.
TEST(PhotoScoring, ThirdsBeatsEdgeClipComposition) {
    const PhotoShot thirds = oneSubject(kThird, kThird);
    const PhotoShot edge = oneSubject(0.99f, 0.99f);
    const float a = ScorePhoto(thirds).composition;
    const float b = ScorePhoto(edge).composition;
    EXPECT_GT(a, b);
}

// A well-composed (thirds) shot beats a dead-center one on the TOTAL too.
TEST(PhotoScoring, WellComposedBeatsCenterTotal) {
    const PhotoShot thirds = oneSubject(kThird, kThird);
    const PhotoShot center = oneSubject(0.0f, 0.0f);
    EXPECT_GT(ScorePhoto(thirds).total, ScorePhoto(center).total);
}

// ---- LIGHTING ----

// A blown-out subject (light ~1) scores lower on LIGHTING than a well-lit one.
TEST(PhotoScoring, BlownOutLosesLighting) {
    const PhotoShot good = oneSubject(kThird, kThird, /*light=*/0.7f);
    const PhotoShot blown = oneSubject(kThird, kThird, /*light=*/0.99f);
    EXPECT_GT(ScorePhoto(good).lighting, ScorePhoto(blown).lighting);
}

// A too-dark subject (light ~0) also scores lower on LIGHTING than a well-lit one.
TEST(PhotoScoring, TooDarkLosesLighting) {
    const PhotoShot good = oneSubject(kThird, kThird, /*light=*/0.7f);
    const PhotoShot dark = oneSubject(kThird, kThird, /*light=*/0.03f);
    EXPECT_GT(ScorePhoto(good).lighting, ScorePhoto(dark).lighting);
}

// A badly-exposed camera (over-exposed) lowers LIGHTING vs a well-exposed one.
TEST(PhotoScoring, OverExposureLosesLighting) {
    const PhotoShot good = oneSubject(kThird, kThird, 0.7f, 0.3f, 1, /*exposure=*/0.5f);
    const PhotoShot over = oneSubject(kThird, kThird, 0.7f, 0.3f, 1, /*exposure=*/0.98f);
    EXPECT_GT(ScorePhoto(good).lighting, ScorePhoto(over).lighting);
}

// ---- FOCUS ----

// An out-of-focus shot scores lower on FOCUS than an otherwise identical sharp one.
TEST(PhotoScoring, OutOfFocusLosesFocus) {
    const PhotoShot sharp = oneSubject(kThird, kThird, 0.7f, 0.3f, 1, 0.5f, /*focus=*/1.0f);
    const PhotoShot blur = oneSubject(kThird, kThird, 0.7f, 0.3f, 1, 0.5f, /*focus=*/0.1f);
    EXPECT_GT(ScorePhoto(sharp).focus, ScorePhoto(blur).focus);
}

// ---- RARITY ----

// A single subject of a kind (rare) scores higher RARITY than a frame packed with
// many of the SAME species (common cluster).
TEST(PhotoScoring, RareBeatsCommonCluster) {
    const PhotoShot rare = oneSubject(kThird, kThird, 0.7f, 0.3f, /*species=*/7);

    PhotoShot common;
    common.exposure = 0.5f;
    common.focus = 1.0f;
    for (int i = 0; i < 5; ++i) {
        PhotoSubject s;
        s.ndc_x = kThird;
        s.ndc_y = kThird;
        s.light = 0.7f;
        s.size = 0.3f;
        s.species_id = 7; // all the same species -> common
        common.subjects.push_back(s);
    }
    EXPECT_GT(ScorePhoto(rare).rarity, ScorePhoto(common).rarity);
}

// Capturing several DISTINCT species lifts RARITY above a same-count single-species
// cluster (variety is prized).
TEST(PhotoScoring, VarietyBeatsMonoculture) {
    PhotoShot variety;
    variety.exposure = 0.5f;
    variety.focus = 1.0f;
    PhotoShot mono;
    mono.exposure = 0.5f;
    mono.focus = 1.0f;
    for (int i = 0; i < 4; ++i) {
        PhotoSubject v;
        v.ndc_x = 0.0f;
        v.ndc_y = 0.0f;
        v.light = 0.7f;
        v.size = 0.3f;
        v.species_id = 100 + i; // four distinct species
        variety.subjects.push_back(v);

        PhotoSubject m;
        m.ndc_x = 0.0f;
        m.ndc_y = 0.0f;
        m.light = 0.7f;
        m.size = 0.3f;
        m.species_id = 100; // all identical
        mono.subjects.push_back(m);
    }
    EXPECT_GT(ScorePhoto(variety).rarity, ScorePhoto(mono).rarity);
}

// ---- bounds ----

// Every axis and the total stay within [0,1] for a strong, ordinary, and degenerate
// shot alike.
TEST(PhotoScoring, ScoresAreClampedToUnitRange) {
    const PhotoShot shots[] = {
        oneSubject(kThird, kThird, 0.7f, 0.9f, 3),          // strong
        oneSubject(0.0f, 0.0f, 0.5f, 0.1f, 1),              // ordinary
        oneSubject(1.0f, -1.0f, 1.0f, 0.0f, 0, 1.0f, 0.0f), // degenerate / clipped
    };
    for (const auto& shot : shots) {
        const PhotoScore sc = ScorePhoto(shot);
        for (float v : {sc.composition, sc.lighting, sc.focus, sc.rarity, sc.total}) {
            EXPECT_GE(v, 0.0f);
            EXPECT_LE(v, 1.0f);
        }
    }
}

// ---- determinism: run == replay ----

// The same PhotoShot yields a BIT-IDENTICAL PhotoScore on every evaluation (pure,
// libm-free, no rng/wall-clock). This is the run==replay contract for the scorer.
TEST(PhotoScoring, RunEqualsReplay) {
    PhotoShot shot;
    shot.exposure = 0.42f;
    shot.focus = 0.83f;
    const struct {
        float x, y, light, size;
        int sp;
    } seed[] = {
        {kThird, -kThird, 0.66f, 0.40f, 3},
        {-0.10f, 0.20f, 0.71f, 0.25f, 9},
        {0.30f, 0.31f, 0.55f, 0.15f, 3},
    };
    for (const auto& d : seed) {
        PhotoSubject s;
        s.ndc_x = d.x;
        s.ndc_y = d.y;
        s.light = d.light;
        s.size = d.size;
        s.species_id = d.sp;
        shot.subjects.push_back(s);
    }

    const PhotoScore a = ScorePhoto(shot);
    const PhotoScore b = ScorePhoto(shot);
    // Exact bit equality (==), not approximate: determinism, not tolerance.
    EXPECT_EQ(a.composition, b.composition);
    EXPECT_EQ(a.lighting, b.lighting);
    EXPECT_EQ(a.focus, b.focus);
    EXPECT_EQ(a.rarity, b.rarity);
    EXPECT_EQ(a.total, b.total);
}

// ---- AETHER_GLOW ----

// Zero aether input (the default) contributes NOTHING: aether_glow is exactly 0
// and the total is BIT-IDENTICAL to the legacy four-axis weighted sum -- the
// byte-neutral proof that every pre-aether caller/shot scores unchanged.
TEST(PhotoScoring, ZeroAetherIsByteNeutral) {
    using luminumbra::photo::Clamp01;
    using luminumbra::photo::kwComposition;
    using luminumbra::photo::kwFocus;
    using luminumbra::photo::kwLighting;
    using luminumbra::photo::kwRarity;

    const PhotoShot shot = oneSubject(kThird, kThird); // aether left at default 0
    const PhotoScore sc = ScorePhoto(shot);
    EXPECT_EQ(sc.aether_glow, 0.0f);
    // Recompute the PRE-AETHER total formula from the returned axes: the new
    // bonus term must have added exactly +0.0f (bit equality, not tolerance).
    const float legacy_total = Clamp01(kwComposition * sc.composition + kwLighting * sc.lighting +
                                       kwFocus * sc.focus + kwRarity * sc.rarity);
    EXPECT_EQ(sc.total, legacy_total);

    // An EXPLICIT zero scores identically to the default (the field is inert at 0).
    PhotoShot explicit_zero = shot;
    explicit_zero.aether = 0.0f;
    const PhotoScore sz = ScorePhoto(explicit_zero);
    EXPECT_EQ(sz.aether_glow, sc.aether_glow);
    EXPECT_EQ(sz.total, sc.total);
}

// A rising aether level lifts aether_glow MONOTONICALLY (non-decreasing) and
// never lowers the total: more energy in frame is never a penalty. Bounded
// [0,1] throughout, including an over-unity input (clamped, still monotone).
TEST(PhotoScoring, AetherGlowMonotonicNonDecreasing) {
    const float levels[] = {0.0f, 0.15f, 0.40f, 0.75f, 1.0f, 1.8f};
    float prev_glow = -1.0f;
    float prev_total = -1.0f;
    for (const float level : levels) {
        PhotoShot shot = oneSubject(kThird, kThird);
        shot.aether = level;
        const PhotoScore sc = ScorePhoto(shot);
        EXPECT_GE(sc.aether_glow, prev_glow) << "level=" << level;
        EXPECT_GE(sc.total, prev_total) << "level=" << level;
        EXPECT_GE(sc.aether_glow, 0.0f);
        EXPECT_LE(sc.aether_glow, 1.0f);
        EXPECT_LE(sc.total, 1.0f);
        prev_glow = sc.aether_glow;
        prev_total = sc.total;
    }

    // And a glowing frame strictly beats the same frame with no energy on the
    // axis (the bonus actually reaches the score, not just non-decreasing).
    PhotoShot dark = oneSubject(kThird, kThird);
    PhotoShot glow = dark;
    glow.aether = 0.8f;
    EXPECT_GT(ScorePhoto(glow).aether_glow, ScorePhoto(dark).aether_glow);
    EXPECT_GT(ScorePhoto(glow).total, ScorePhoto(dark).total);
}

} // namespace
