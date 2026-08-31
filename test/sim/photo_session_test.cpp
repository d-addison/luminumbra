// game.photo_session: the photography-loop GLUE. These tests pin the
// blended verdict + codex feed: a well-composed, well-exposed, sharp shallow-DoF
// shot scores HIGH with many stars; a blown-out / out-of-focus shot scores LOWER;
// stars are a monotonic function of the total; CommitShot records the MAIN species
// into the codex with the verdict's total and keeps the BEST across repeated shots;
// and the whole flow is deterministic (run==replay). PURE: no entt, no rng, no
// wall-clock — EvaluateShot/CommitShot operate on plain value structs and compose
// the existing PhotoScoring / PhotoCamera / PhotoCodex libraries.
#include <gtest/gtest.h>

#include <vector>

#include "game/PhotoSession.h"

namespace {

using luminumbra::game::CommitShot;
using luminumbra::game::EvaluateShot;
using luminumbra::game::LensSettings;
using luminumbra::game::PhotoCodex;
using luminumbra::game::ShotInput;
using luminumbra::game::ShotVerdict;
using luminumbra::game::StarsForTotal;
namespace photo = ::luminumbra::photo;

// ---------------------------------------------------------------------------
// Builders for canonical shots.
// ---------------------------------------------------------------------------

// A GREAT shot: main subject sitting on a rule-of-thirds power point, mid-high
// luminance, sharp focus, mid exposure; a wide-aperture short-tele lens focused
// right on a subject at the focus distance (shallow DoF, subject in focus); scene
// luminance chosen so the lens EV matches the target.
ShotInput MakeGreatShot() {
    ShotInput in;
    photo::PhotoSubject s;
    s.ndc_x = luminumbra::photo::kThird; // on a power point
    s.ndc_y = luminumbra::photo::kThird;
    s.size = 0.6f;  // bold, prominent
    s.light = 0.7f; // ideal luminance
    s.species_id = 42;
    in.composition.subjects.push_back(s);
    in.composition.exposure = 0.5f; // ideal mid exposure
    in.composition.focus = 1.0f;    // tack sharp

    // Wide aperture short-tele, focused ON the subject -> shallow DoF, in focus.
    in.lens.focal_length_mm = 85.0f;
    in.lens.aperture_f = 1.8f;
    in.lens.focus_distance_m = 3.0f;
    in.lens.iso = 100.0f;
    in.lens.shutter_s = 0.004f; // ~1/250

    in.main_species_id = 42;
    in.main_subject_distance_m = 3.0f; // exactly at focus distance
    in.main_subject_size_m = 0.6f;
    in.scene_luminance = 0.5f;
    return in;
}

// A BAD shot: subject jammed into the frame corner (edge-clipped, off the thirds),
// BLOWN OUT luminance, fully out of focus; a deep-stopped lens focused far from the
// subject (everything sharp -> no isolation, AND subject out of the DoF band); the
// lens EV badly mismatched to a near-dark scene.
ShotInput MakeBadShot() {
    ShotInput in;
    photo::PhotoSubject s;
    s.ndc_x = 0.97f; // jammed into the right edge (clipped, off thirds)
    s.ndc_y = 0.97f;
    s.size = 0.05f;  // tiny, weak
    s.light = 0.99f; // blown out
    s.species_id = 7;
    in.composition.subjects.push_back(s);
    in.composition.exposure = 0.98f; // wildly over-exposed
    in.composition.focus = 0.05f;    // out of focus

    // Deep stop, focused at infinity, subject very close -> subject outside DoF.
    in.lens.focal_length_mm = 24.0f;
    in.lens.aperture_f = 22.0f;
    in.lens.focus_distance_m = 100.0f;
    in.lens.iso = 100.0f;
    in.lens.shutter_s = 0.001f; // very fast -> high EV, wrong for dark scene

    in.main_species_id = 7;
    in.main_subject_distance_m = 0.5f; // close, far from the 100m focus
    in.main_subject_size_m = 0.1f;
    in.scene_luminance = 0.02f; // near-dark scene
    return in;
}

// ---------------------------------------------------------------------------
// Core behaviour.
// ---------------------------------------------------------------------------

// A great shot scores high across axes and earns many stars.
TEST(PhotoSession, GreatShotScoresHighWithManyStars) {
    const ShotVerdict v = EvaluateShot(MakeGreatShot());

    EXPECT_GT(v.composition, 0.7f);
    EXPECT_GT(v.exposure, 0.6f);
    EXPECT_GT(v.focus_isolation, 0.6f);
    EXPECT_GT(v.total, 0.6f);
    EXPECT_GE(v.stars, 3);
}

// A blown-out / out-of-focus / badly-framed shot scores clearly lower and earns
// few stars.
TEST(PhotoSession, BadShotScoresLowWithFewStars) {
    const ShotVerdict v = EvaluateShot(MakeBadShot());

    EXPECT_LT(v.total, 0.35f);
    EXPECT_LE(v.stars, 1);
}

// The bad shot scores strictly lower than the great shot on every axis and total.
TEST(PhotoSession, GreatBeatsBad) {
    const ShotVerdict good = EvaluateShot(MakeGreatShot());
    const ShotVerdict bad = EvaluateShot(MakeBadShot());

    EXPECT_GT(good.composition, bad.composition);
    EXPECT_GT(good.exposure, bad.exposure);
    EXPECT_GT(good.focus_isolation, bad.focus_isolation);
    EXPECT_GT(good.total, bad.total);
    EXPECT_GE(good.stars, bad.stars);
}

// REBALANCE GUARD: focus/optics now lead, but composition must still
// anchor the verdict — a well-composed, sharp DEEP shot (f/8, poor isolation) beats a
// badly-composed shallow one (f/1.4, creamy bokeh) so lens craft cannot buy a win on a
// poorly-framed subject. Without the composition anchor the focus tilt would invert this.
TEST(PhotoSession, WellComposedDeepBeatsBadlyComposedShallow) {
    // Well composed, deep stop (f/8): bold subject on a thirds power point, sharp,
    // ideal light; everything-sharp lens => low isolation.
    ShotInput deep;
    {
        photo::PhotoSubject s;
        s.ndc_x = luminumbra::photo::kThird;
        s.ndc_y = luminumbra::photo::kThird;
        s.size = 0.6f;
        s.light = 0.7f;
        s.species_id = 1;
        deep.composition.subjects.push_back(s);
        deep.composition.exposure = 0.5f;
        deep.composition.focus = 1.0f;
        deep.lens.focal_length_mm = 50.0f;
        deep.lens.aperture_f = 8.0f;
        deep.lens.focus_distance_m = 3.0f;
        deep.lens.iso = 100.0f;
        deep.lens.shutter_s = 0.004f;
        deep.main_species_id = 1;
        deep.main_subject_distance_m = 3.0f;
        deep.main_subject_size_m = 0.6f;
        deep.scene_luminance = 0.5f;
    }
    // Badly composed, wide open (f/1.4): subject jammed in the corner, tiny, but tack
    // sharp with a creamy shallow DoF on it.
    ShotInput shallow;
    {
        photo::PhotoSubject s;
        s.ndc_x = 0.97f;
        s.ndc_y = 0.97f;
        s.size = 0.06f;
        s.light = 0.7f;
        s.species_id = 2;
        shallow.composition.subjects.push_back(s);
        shallow.composition.exposure = 0.5f;
        shallow.composition.focus = 1.0f;
        shallow.lens.focal_length_mm = 85.0f;
        shallow.lens.aperture_f = 1.4f;
        shallow.lens.focus_distance_m = 3.0f;
        shallow.lens.iso = 100.0f;
        shallow.lens.shutter_s = 0.004f;
        shallow.main_species_id = 2;
        shallow.main_subject_distance_m = 3.0f;
        shallow.main_subject_size_m = 0.5f;
        shallow.scene_luminance = 0.5f;
    }

    const ShotVerdict d = EvaluateShot(deep);
    const ShotVerdict h = EvaluateShot(shallow);
    EXPECT_GT(d.focus_isolation, 0.0f);
    EXPECT_GT(h.focus_isolation, d.focus_isolation); // the shallow shot DOES win the optics axis
    EXPECT_GT(d.composition, h.composition);         //...but loses framing badly
    EXPECT_GT(d.total, h.total);                     // and composition anchors the overall win
}

// CommitShot folds the capture's ObservationMetadata.subject_action into the codex so
// the behavioural progression layer can later match it. A plain capture sets no bit.
TEST(PhotoSession, CommitCarriesBehaviourIntoCodex) {
    PhotoCodex codex;
    ShotInput in = MakeGreatShot(); // species 42

    // Plain capture: no behaviour recorded.
    CommitShot(codex, in, EvaluateShot(in));
    EXPECT_FALSE(codex.behavior_captured_any(5));

    // A capture annotated as the subject SLEEPING (brain action 5) sets the bit.
    in.observation.subject_action = 5;
    CommitShot(codex, in, EvaluateShot(in));
    EXPECT_TRUE(codex.behavior_captured_any(5));
    EXPECT_TRUE(codex.behavior_captured(42, 5));
    EXPECT_FALSE(codex.behavior_captured(42, 3)); // never captured hunting
}

// All verdict axes + total stay in [0,1].
TEST(PhotoSession, VerdictAxesAreClamped) {
    for (const ShotInput& in : {MakeGreatShot(), MakeBadShot()}) {
        const ShotVerdict v = EvaluateShot(in);
        EXPECT_GE(v.composition, 0.0f);
        EXPECT_LE(v.composition, 1.0f);
        EXPECT_GE(v.exposure, 0.0f);
        EXPECT_LE(v.exposure, 1.0f);
        EXPECT_GE(v.focus_isolation, 0.0f);
        EXPECT_LE(v.focus_isolation, 1.0f);
        EXPECT_GE(v.total, 0.0f);
        EXPECT_LE(v.total, 1.0f);
        EXPECT_GE(v.stars, 0);
        EXPECT_LE(v.stars, 5);
    }
}

// ---------------------------------------------------------------------------
// Stars are a monotonic function of total.
// ---------------------------------------------------------------------------
TEST(PhotoSession, StarsAreMonotonicInTotal) {
    int prev = StarsForTotal(0.0f);
    EXPECT_EQ(prev, 0);
    // Sweep total from 0 to 1; stars must never decrease as total rises.
    for (int i = 1; i <= 1000; ++i) {
        const float t = static_cast<float>(i) / 1000.0f;
        const int stars = StarsForTotal(t);
        EXPECT_GE(stars, prev) << "stars decreased at t=" << t;
        EXPECT_GE(stars, 0);
        EXPECT_LE(stars, 5);
        prev = stars;
    }
    EXPECT_EQ(StarsForTotal(1.0f), 5);
}

// Stars hit each documented threshold band.
TEST(PhotoSession, StarThresholds) {
    EXPECT_EQ(StarsForTotal(0.10f), 0);
    EXPECT_EQ(StarsForTotal(0.20f), 1);
    EXPECT_EQ(StarsForTotal(0.40f), 2);
    EXPECT_EQ(StarsForTotal(0.60f), 3);
    EXPECT_EQ(StarsForTotal(0.75f), 4);
    EXPECT_EQ(StarsForTotal(0.88f), 5);
}

// ---------------------------------------------------------------------------
// CommitShot feeds the codex.
// ---------------------------------------------------------------------------

// CommitShot records the MAIN species with the verdict's total score and discovers it.
TEST(PhotoSession, CommitRecordsSpeciesWithVerdictScore) {
    PhotoCodex codex;
    const ShotInput in = MakeGreatShot();
    const ShotVerdict v = EvaluateShot(in);

    EXPECT_FALSE(codex.discovered(in.main_species_id));
    CommitShot(codex, in, v);

    EXPECT_TRUE(codex.discovered(in.main_species_id));
    ASSERT_EQ(codex.species_count(), 1u);
    const auto& e = codex.entries().front();
    EXPECT_EQ(e.species_id, in.main_species_id);
    EXPECT_EQ(e.captures, 1u);
    EXPECT_FLOAT_EQ(e.best_score, v.total);
}

// Repeated captures of the same species keep the BEST verdict score; a worse later
// shot never lowers the record but still counts as a capture.
TEST(PhotoSession, CommitKeepsBestAcrossRepeatedShots) {
    PhotoCodex codex;

    const ShotInput good = MakeGreatShot(); // species 42, high total
    const ShotVerdict gv = EvaluateShot(good);

    // A worse shot of the SAME species (id 42) — reuse the bad framing but re-key it.
    ShotInput worse = MakeBadShot();
    worse.composition.subjects.front().species_id = 42;
    worse.main_species_id = 42;
    const ShotVerdict wv = EvaluateShot(worse);
    ASSERT_LT(wv.total, gv.total);

    // Commit the worse one first, then the better one.
    CommitShot(codex, worse, wv);
    CommitShot(codex, good, gv);

    ASSERT_EQ(codex.species_count(), 1u);
    const auto& e = codex.entries().front();
    EXPECT_EQ(e.species_id, 42);
    EXPECT_EQ(e.captures, 2u);
    EXPECT_FLOAT_EQ(e.best_score, gv.total); // best is retained

    // Commit a THIRD, worse shot: best stays, captures climbs.
    CommitShot(codex, worse, wv);
    EXPECT_EQ(codex.entries().front().captures, 3u);
    EXPECT_FLOAT_EQ(codex.entries().front().best_score, gv.total);
}

// Distinct species land as distinct codex entries.
TEST(PhotoSession, CommitDistinctSpecies) {
    PhotoCodex codex;
    const ShotInput a = MakeGreatShot(); // species 42
    const ShotInput b = MakeBadShot();   // species 7
    CommitShot(codex, a, EvaluateShot(a));
    CommitShot(codex, b, EvaluateShot(b));
    EXPECT_EQ(codex.species_count(), 2u);
    EXPECT_TRUE(codex.discovered(42));
    EXPECT_TRUE(codex.discovered(7));
}

// ---------------------------------------------------------------------------
// Defined edge case: a subject-less frame is a defined low verdict, not NaN/inf.
// ---------------------------------------------------------------------------
TEST(PhotoSession, EmptyFrameIsDefinedLowVerdict) {
    ShotInput in; // no subjects
    in.main_species_id = 1;
    const ShotVerdict v = EvaluateShot(in);

    // ScorePhoto returns all-zero for an empty frame; composition rides on it.
    EXPECT_FLOAT_EQ(v.composition, 0.0f);
    // Total stays finite + in range and earns no/few stars.
    EXPECT_GE(v.total, 0.0f);
    EXPECT_LE(v.total, 1.0f);
    EXPECT_LE(v.stars, 1);
}

// ---------------------------------------------------------------------------
// run == replay: identical input yields a byte-identical verdict, and replaying
// the same capture sequence yields an identical codex state.
// ---------------------------------------------------------------------------
TEST(PhotoSession, RunEqualsReplay_Verdict) {
    const ShotInput in = MakeGreatShot();
    const ShotVerdict a = EvaluateShot(in);
    const ShotVerdict b = EvaluateShot(in);
    EXPECT_EQ(a.composition, b.composition);
    EXPECT_EQ(a.exposure, b.exposure);
    EXPECT_EQ(a.focus_isolation, b.focus_isolation);
    EXPECT_EQ(a.total, b.total);
    EXPECT_EQ(a.stars, b.stars);
}

TEST(PhotoSession, RunEqualsReplay_Codex) {
    const ShotInput a = MakeGreatShot(); // species 42
    const ShotInput b = MakeBadShot();   // species 7

    PhotoCodex run;
    CommitShot(run, a, EvaluateShot(a));
    CommitShot(run, b, EvaluateShot(b));

    PhotoCodex replay;
    CommitShot(replay, a, EvaluateShot(a));
    CommitShot(replay, b, EvaluateShot(b));

    ASSERT_EQ(run.species_count(), replay.species_count());
    const auto& re = run.entries();
    const auto& pe = replay.entries();
    ASSERT_EQ(re.size(), pe.size());
    for (std::size_t i = 0; i < re.size(); ++i) {
        EXPECT_EQ(re[i].species_id, pe[i].species_id);
        EXPECT_EQ(re[i].captures, pe[i].captures);
        EXPECT_EQ(re[i].best_score, pe[i].best_score);
    }
    EXPECT_EQ(run.total_score(), replay.total_score());
}

} // namespace
