// game.photo_camera: a PURE, DETERMINISTIC camera/lens model for the
// photography loop (photography): focal length + aperture -> depth of field, exposure
// value/quality, and a bokeh/subject-isolation score. No render/GL/entt/rng. These
// tests pin the optics: a wide aperture (low f) gives a SHALLOW DoF and high
// isolation; a small aperture (high f) gives a DEEP DoF; a subject at the focus
// distance is in_focus; exposure quality peaks at a matched scene luminance and
// falls for blown/dark; EV is monotonic in aperture/shutter; and it is a pure
// function (same input -> same output, run==replay).
#include <gtest/gtest.h>

#include "game/PhotoCamera.h"

namespace {

using luminumbra::game::CameraSubject;
using luminumbra::game::ComputeDof;
using luminumbra::game::DofResult;
using luminumbra::game::ExposureQuality;
using luminumbra::game::ExposureValue;
using luminumbra::game::kInfiniteFar;
using luminumbra::game::LensSettings;
using luminumbra::game::Log2Approx;
using luminumbra::game::SubjectIsolation;

// A baseline 50mm lens focused at 3m; aperture is varied per test.
LensSettings lens(float aperture_f, float focus_m = 3.0f, float focal_mm = 50.0f) {
    LensSettings l;
    l.focal_length_mm = focal_mm;
    l.aperture_f = aperture_f;
    l.focus_distance_m = focus_m;
    l.iso = 100.0f;
    l.shutter_s = 0.008f;
    return l;
}

CameraSubject subjectAt(float distance_m, float size_m = 0.5f) {
    CameraSubject s;
    s.distance_m = distance_m;
    s.size_m = size_m;
    return s;
}

float dofBand(const DofResult& d) {
    return d.far_limit_m - d.near_limit_m;
}

// ---- Log2 approximation accuracy (the one libm-free transcendental) ----

// Log2Approx hits the exact powers of two and stays within its documented band.
TEST(PhotoCamera, Log2ApproxExactAtPowersOfTwo) {
    EXPECT_NEAR(Log2Approx(1.0f), 0.0f, 1.0e-3f);
    EXPECT_NEAR(Log2Approx(2.0f), 1.0f, 5.0e-3f);
    EXPECT_NEAR(Log2Approx(4.0f), 2.0f, 5.0e-3f);
    EXPECT_NEAR(Log2Approx(8.0f), 3.0f, 5.0e-3f);
    EXPECT_NEAR(Log2Approx(0.5f), -1.0f, 5.0e-3f);
}

// Intermediate value: log2(3) ~= 1.585, within the ~3.5e-3 band.
TEST(PhotoCamera, Log2ApproxMidOctave) {
    EXPECT_NEAR(Log2Approx(3.0f), 1.5849625f, 4.0e-3f);
    EXPECT_NEAR(Log2Approx(6.0f), 2.5849625f, 4.0e-3f);
}

// Non-positive inputs are a defined finite floor (no NaN/-inf).
TEST(PhotoCamera, Log2ApproxNonPositiveIsFinite) {
    const float z = Log2Approx(0.0f);
    const float n = Log2Approx(-5.0f);
    EXPECT_TRUE(z == z); // not NaN
    EXPECT_TRUE(n == n);
    EXPECT_GT(z, -1.0e30f); // finite floor, not -inf
    EXPECT_GT(n, -1.0e30f);
}

// ---- Depth of field ----

// A wide aperture (low f) gives a SHALLOW DoF; a small aperture (high f) gives a
// DEEP DoF. The narrow band must be strictly smaller than the wide band.
TEST(PhotoCamera, WideApertureShallowDof_SmallApertureDeepDof) {
    const CameraSubject subj = subjectAt(3.0f);
    const DofResult wide = ComputeDof(lens(/*f=*/1.4f), subj);
    const DofResult deep = ComputeDof(lens(/*f=*/16.0f), subj);

    const float wide_band = dofBand(wide);
    const float deep_band = dofBand(deep);

    EXPECT_GT(wide_band, 0.0f);
    EXPECT_GT(deep_band, wide_band) << "f/16 must yield a deeper DoF than f/1.4";

    // Near/far bracket the focus distance for a normal (sub-hyperfocal) shot.
    EXPECT_LT(wide.near_limit_m, 3.0f);
    EXPECT_GT(wide.far_limit_m, 3.0f);
}

// A subject AT the focus distance is in focus regardless of aperture.
TEST(PhotoCamera, SubjectAtFocusDistanceIsInFocus) {
    const CameraSubject subj = subjectAt(3.0f);
    EXPECT_FLOAT_EQ(ComputeDof(lens(1.4f), subj).in_focus, 1.0f);
    EXPECT_FLOAT_EQ(ComputeDof(lens(16.0f), subj).in_focus, 1.0f);
}

// A subject far outside a shallow DoF band is NOT in focus.
TEST(PhotoCamera, SubjectOutsideShallowDofIsNotInFocus) {
    // Wide-open, focused at 3m; a subject at 30m is well beyond the far limit.
    const DofResult d = ComputeDof(lens(/*f=*/1.4f), subjectAt(30.0f));
    EXPECT_LT(d.far_limit_m, 30.0f);
    EXPECT_FLOAT_EQ(d.in_focus, 0.0f);
}

// Focusing at/beyond the hyperfocal distance makes the far limit "infinite".
TEST(PhotoCamera, HyperfocalGivesInfiniteFar) {
    // Small aperture + far focus pushes past hyperfocal -> far = sentinel.
    LensSettings l = lens(/*f=*/22.0f, /*focus=*/100.0f);
    const DofResult d = ComputeDof(l, subjectAt(100.0f));
    EXPECT_GE(d.far_limit_m, kInfiniteFar);
    EXPECT_FLOAT_EQ(d.in_focus, 1.0f); // subject at focus, DoF to infinity
}

// Degenerate lens (non-positive focal length) is handled to a defined result.
TEST(PhotoCamera, DegenerateLensIsDefined) {
    LensSettings l = lens(2.8f);
    l.focal_length_mm = 0.0f;
    const DofResult d = ComputeDof(l, subjectAt(3.0f));
    EXPECT_FLOAT_EQ(d.near_limit_m, 3.0f);
    EXPECT_FLOAT_EQ(d.far_limit_m, 3.0f);
    EXPECT_FLOAT_EQ(d.in_focus, 1.0f); // subject exactly at focus distance
}

// ---- Exposure value monotonicity ----

// EV RISES as the aperture stops down (larger f-number) — all else equal.
TEST(PhotoCamera, EvMonotonicIncreasingInAperture) {
    const float ev_wide = ExposureValue(lens(/*f=*/2.0f));
    const float ev_mid = ExposureValue(lens(/*f=*/4.0f));
    const float ev_narrow = ExposureValue(lens(/*f=*/8.0f));
    EXPECT_LT(ev_wide, ev_mid);
    EXPECT_LT(ev_mid, ev_narrow);
    // One stop of f-number (sqrt2 ~ x2 area) = ~2 EV per f-number doubling.
    EXPECT_NEAR(ev_narrow - ev_wide, 4.0f, 0.1f); // f/2 -> f/8 = two doublings.
}

// EV RISES as shutter gets faster (shorter time) — all else equal.
TEST(PhotoCamera, EvMonotonicIncreasingWithFasterShutter) {
    LensSettings slow = lens(2.8f);
    slow.shutter_s = 0.016f; // 1/60
    LensSettings fast = lens(2.8f);
    fast.shutter_s = 0.004f; // 1/250 (two stops faster)
    const float ev_slow = ExposureValue(slow);
    const float ev_fast = ExposureValue(fast);
    EXPECT_GT(ev_fast, ev_slow);
    EXPECT_NEAR(ev_fast - ev_slow, 2.0f, 0.1f); // 4x shorter time = 2 EV.
}

// Raising ISO LOWERS the EV the scene must supply.
TEST(PhotoCamera, EvDecreasesWithHigherIso) {
    LensSettings base = lens(2.8f);
    LensSettings high = lens(2.8f);
    high.iso = 400.0f; // two stops more sensitive
    EXPECT_GT(ExposureValue(base), ExposureValue(high));
    EXPECT_NEAR(ExposureValue(base) - ExposureValue(high), 2.0f, 0.1f);
}

// ---- Exposure quality ----

// Quality peaks when the lens EV matches the EV the scene luminance wants, and
// falls off for both an over-exposed (blown) and under-exposed (dark) frame.
TEST(PhotoCamera, ExposureQualityPeaksAtMatchedLuminance) {
    LensSettings l = lens(2.8f);
    const float lens_ev = ExposureValue(l);

    // Find the scene luminance whose target EV equals this lens EV:
    //   target_ev = kSceneEvMin + lum*kSceneEvSpan  =>  lum = (ev - 6)/9.
    const float matched_lum = (lens_ev - 6.0f) / 9.0f;
    const float clamped = matched_lum < 0.0f ? 0.0f : (matched_lum > 1.0f ? 1.0f : matched_lum);

    const float q_match = ExposureQuality(l, clamped);
    const float q_bright = ExposureQuality(l, 1.0f); // very bright scene -> dark frame
    const float q_dark = ExposureQuality(l, 0.0f);   // very dark scene -> blown frame

    EXPECT_GT(q_match, q_bright);
    EXPECT_GT(q_match, q_dark);
    EXPECT_GE(q_match, 0.9f); // near the matched point, quality is high
    // All quality scores are within [0,1].
    EXPECT_GE(q_match, 0.0f);
    EXPECT_LE(q_match, 1.0f);
    EXPECT_GE(q_bright, 0.0f);
    EXPECT_LE(q_bright, 1.0f);
    EXPECT_GE(q_dark, 0.0f);
    EXPECT_LE(q_dark, 1.0f);
}

// ---- Subject isolation ----

// A wide aperture with the subject in focus isolates it FAR better than a small
// aperture with everything sharp.
TEST(PhotoCamera, WideApertureIsolatesSubjectMoreThanSmall) {
    const CameraSubject subj = subjectAt(3.0f);
    const float iso_wide = SubjectIsolation(lens(/*f=*/1.4f), subj);
    const float iso_deep = SubjectIsolation(lens(/*f=*/16.0f), subj);
    EXPECT_GT(iso_wide, iso_deep);
    EXPECT_GE(iso_wide, 0.0f);
    EXPECT_LE(iso_wide, 1.0f);
    EXPECT_GE(iso_deep, 0.0f);
    EXPECT_LE(iso_deep, 1.0f);
}

// A subject OUT of focus collapses isolation to a small residual even wide-open.
TEST(PhotoCamera, OutOfFocusSubjectHasLowIsolation) {
    // Wide-open focused at 3m, subject at 30m (out of the shallow band).
    const float iso_missed = SubjectIsolation(lens(/*f=*/1.4f), subjectAt(30.0f));
    const float iso_hit = SubjectIsolation(lens(/*f=*/1.4f), subjectAt(3.0f));
    EXPECT_LT(iso_missed, iso_hit);
    EXPECT_LT(iso_missed, 0.3f);
}

// ---- Purity / run==replay ----

// The model is a pure function: identical inputs yield bit-identical outputs.
TEST(PhotoCamera, RunEqualsReplay) {
    const LensSettings l = lens(/*f=*/1.8f, /*focus=*/4.5f, /*focal=*/85.0f);
    const CameraSubject subj = subjectAt(4.5f, 0.6f);

    for (int i = 0; i < 5; ++i) {
        const DofResult d1 = ComputeDof(l, subj);
        const DofResult d2 = ComputeDof(l, subj);
        EXPECT_EQ(d1.near_limit_m, d2.near_limit_m);
        EXPECT_EQ(d1.far_limit_m, d2.far_limit_m);
        EXPECT_EQ(d1.in_focus, d2.in_focus);

        EXPECT_EQ(ExposureValue(l), ExposureValue(l));
        EXPECT_EQ(ExposureQuality(l, 0.4f), ExposureQuality(l, 0.4f));
        EXPECT_EQ(SubjectIsolation(l, subj), SubjectIsolation(l, subj));
    }
}

} // namespace
