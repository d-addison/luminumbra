// Cluster: PHOTOGRAPHY / GAME LIBRARIES (pure, no entt) — ADVERSARIAL HARDENING.
//
// These tests probe what the per-system happy-path suites MISS: bounds/NaN on
// degenerate input, monotonicity claims, producer->consumer contracts between the
// libraries, codex/registry best-keeping + sorted determinism, star-threshold
// monotonicity, Log2Approx accuracy across octaves + non-positive guards, and
// run==replay purity. The systems are -ffp-contract=off bit-exact, so run==replay
// asserts EXACT float equality; approximation-bearing claims (DM::Cos falloffs,
// Log2Approx) use a documented tolerance band instead.
//
// IMPORTANT COMPILE CONSTRAINT (see BUG: gamelibs_clamp01_odr_collision below):
// game/PhotoFilters.h and game/DifficultyProfile.h each (re)define a NON-static
// inline `luminumbra::game::Clamp01(float)` — the SAME unqualified name PhotoCamera.h
// already defines in the SAME namespace. Two of those headers in one translation
// unit is a hard C++ redefinition error (an ODR violation), so this file CANNOT
// include PhotoFilters.h / DifficultyProfile.h alongside the PhotoCamera/PhotoSession
// core loop. It therefore hardens the maximal CO-COMPILABLE set (PhotoScoring,
// PhotoCamera, PhotoCodex, SpeciesCodex, LightTools, PhotoSession — the whole core
// photography loop). Once the ODR collision is fixed (make those Clamp01s file-local
// / detail-namespaced, or share one helper), PhotoFilters + DifficultyProfile tests
// can be folded in here.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "game/LightTools.h"      // luminumbra::game
#include "game/PhotoCamera.h"     // luminumbra::game
#include "game/PhotoCodex.h"      // luminumbra::game
#include "game/PhotoSession.h"    // luminumbra::game
#include "game/SpeciesCodex.h"    // luminumbra::game
#include "systems/PhotoScoring.h" // luminumbra::photo

namespace {

namespace ph = ::luminumbra::photo;
namespace gm = ::luminumbra::game;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
bool IsFiniteF(float v) {
    // No std::isfinite to keep the determinism flavour explicit: NaN != NaN, and
    // +-inf compare out of any finite band.
    if (v != v)
        return false;                  // NaN
    if (v > 3.0e38f || v < -3.0e38f) { // beyond finite float range (inf)
        return false;
    }
    return true;
}

void ExpectScoreInRange(float v, const char* what) {
    EXPECT_TRUE(IsFiniteF(v)) << what << " not finite: " << v;
    EXPECT_GE(v, 0.0f) << what << " below 0: " << v;
    EXPECT_LE(v, 1.0f) << what << " above 1: " << v;
}

ph::PhotoSubject MakeSubject(float x, float y, float size, float light, int id) {
    ph::PhotoSubject s;
    s.ndc_x = x;
    s.ndc_y = y;
    s.size = size;
    s.light = light;
    s.species_id = id;
    return s;
}

// =====================================================================================
//  PhotoScoring.h  (luminumbra::photo)
// =====================================================================================

// Empty frame is a strictly-defined all-zero score (the documented gating contract).
TEST(GameLibsHardening, ScorePhotoEmptyFrameAllZero) {
    ph::PhotoShot shot; // no subjects
    const ph::PhotoScore s = ph::ScorePhoto(shot);
    EXPECT_EQ(s.composition, 0.0f);
    EXPECT_EQ(s.lighting, 0.0f);
    EXPECT_EQ(s.focus, 0.0f);
    EXPECT_EQ(s.rarity, 0.0f);
    EXPECT_EQ(s.total, 0.0f);
}

// Degenerate subjects (zero size / zero or extreme light / coincident at origin)
// must produce finite, clamped axes — never NaN/inf from a /0 or a blow-out branch.
TEST(GameLibsHardening, ScorePhotoDegenerateInputsFiniteAndClamped) {
    const float lights[] = {-1.0f, 0.0f, 0.5f, 0.92f, 1.0f, 2.0f};
    const float sizes[] = {0.0f, 1.0f, 5.0f};
    const float expos[] = {-1.0f, 0.0f, 0.5f, 1.0f, 2.0f};
    for (float l : lights) {
        for (float sz : sizes) {
            for (float ex : expos) {
                ph::PhotoShot shot;
                shot.exposure = ex;
                shot.focus = ex; // reuse the sweep value as a focus too
                // two coincident subjects at the dead centre, same species.
                shot.subjects.push_back(MakeSubject(0.0f, 0.0f, sz, l, 7));
                shot.subjects.push_back(MakeSubject(0.0f, 0.0f, sz, l, 7));
                const ph::PhotoScore s = ph::ScorePhoto(shot);
                ExpectScoreInRange(s.composition, "composition");
                ExpectScoreInRange(s.lighting, "lighting");
                ExpectScoreInRange(s.focus, "focus");
                ExpectScoreInRange(s.rarity, "rarity");
                ExpectScoreInRange(s.total, "total");
            }
        }
    }
}

// A blown-out subject (light -> 1) must score LOWER on lighting than a well-lit one
// at the ideal luminance (the blow-out penalty must actually bite, not be swamped).
TEST(GameLibsHardening, ScoreLightingBlowoutPenalised) {
    ph::PhotoShot ideal;
    ideal.exposure = 0.5f;
    ideal.subjects.push_back(MakeSubject(0.33333334f, 0.33333334f, 0.6f, 0.7f, 1));
    ph::PhotoShot blown = ideal;
    blown.subjects[0].light = 1.0f;
    EXPECT_LT(ph::ScorePhoto(blown).lighting, ph::ScorePhoto(ideal).lighting);
}

// Composition is order-INDEPENDENT when subjects have DISTINCT sizes: shuffling the
// subject vector must not change any axis (results keyed by geometry, not order).
TEST(GameLibsHardening, ScorePhotoOrderIndependentDistinctSizes) {
    std::vector<ph::PhotoSubject> subs = {
        MakeSubject(0.33f, 0.33f, 0.6f, 0.7f, 1),
        MakeSubject(-0.5f, 0.2f, 0.3f, 0.5f, 2),
        MakeSubject(0.1f, -0.4f, 0.2f, 0.4f, 3),
    };
    ph::PhotoShot a;
    a.subjects = subs;
    a.exposure = 0.5f;
    a.focus = 0.9f;
    ph::PhotoShot b;
    b.subjects.assign(subs.rbegin(), subs.rend());
    b.exposure = 0.5f;
    b.focus = 0.9f;
    const ph::PhotoScore sa = ph::ScorePhoto(a);
    const ph::PhotoScore sb = ph::ScorePhoto(b);
    EXPECT_EQ(sa.composition, sb.composition);
    EXPECT_EQ(sa.lighting, sb.lighting);
    EXPECT_EQ(sa.focus, sb.focus);
    EXPECT_EQ(sa.rarity, sb.rarity);
    EXPECT_EQ(sa.total, sb.total);
}

// Rarity is order-independent AND rewards variety: N distinct species must out-score
// N of the SAME species (variety bonus), and the value must not depend on order.
TEST(GameLibsHardening, ScoreRarityVarietyAndOrderIndependent) {
    ph::PhotoShot same;
    for (int i = 0; i < 4; ++i)
        same.subjects.push_back(MakeSubject(0.0f, 0.0f, 0.3f, 0.5f, 9));
    ph::PhotoShot varied;
    for (int i = 0; i < 4; ++i)
        varied.subjects.push_back(MakeSubject(0.0f, 0.0f, 0.3f, 0.5f, 9 + i));
    EXPECT_GT(ph::ScoreRarity(varied), ph::ScoreRarity(same));

    ph::PhotoShot v1;
    v1.subjects = {MakeSubject(0, 0, 0.5f, 0.5f, 1),
                   MakeSubject(0.1f, 0.1f, 0.3f, 0.5f, 2),
                   MakeSubject(0.2f, 0.2f, 0.2f, 0.5f, 2)};
    ph::PhotoShot v2;
    v2.subjects = {MakeSubject(0.2f, 0.2f, 0.2f, 0.5f, 2),
                   MakeSubject(0, 0, 0.5f, 0.5f, 1),
                   MakeSubject(0.1f, 0.1f, 0.3f, 0.5f, 2)};
    EXPECT_EQ(ph::ScoreRarity(v1), ph::ScoreRarity(v2));
}

// run==replay: the same PhotoShot scored twice is byte-identical (pure function).
TEST(GameLibsHardening, ScorePhotoRunEqualsReplay) {
    ph::PhotoShot shot;
    shot.exposure = 0.42f;
    shot.focus = 0.83f;
    shot.subjects = {MakeSubject(0.31f, -0.34f, 0.55f, 0.66f, 4),
                     MakeSubject(-0.2f, 0.5f, 0.25f, 0.4f, 7)};
    const ph::PhotoScore a = ph::ScorePhoto(shot);
    const ph::PhotoScore b = ph::ScorePhoto(shot);
    EXPECT_EQ(a.composition, b.composition);
    EXPECT_EQ(a.lighting, b.lighting);
    EXPECT_EQ(a.focus, b.focus);
    EXPECT_EQ(a.rarity, b.rarity);
    EXPECT_EQ(a.total, b.total);
}

// =====================================================================================
//  PhotoCamera.h  (luminumbra::game)
// =====================================================================================

// Log2Approx accuracy across many octaves, exact at powers of two, and finite (never
// -inf/NaN) for zero/negative inputs. The header claims a ~4e-3 accuracy band.
TEST(GameLibsHardening, Log2ApproxAccuracyBandAcrossOctaves) {
    // Exact (to the documented band) at integer powers of two: log2(2^e) == e.
    for (int e = -10; e <= 12; ++e) {
        float x = 1.0f;
        if (e >= 0) {
            for (int k = 0; k < e; ++k)
                x *= 2.0f;
        } else {
            for (int k = 0; k < -e; ++k)
                x *= 0.5f;
        }
        const float got = gm::Log2Approx(x);
        EXPECT_TRUE(IsFiniteF(got));
        EXPECT_NEAR(got, static_cast<float>(e), 4.0e-3f) << "log2(2^" << e << ")";
    }
    // Across the [1,2) octave the polynomial must hold its band against a reference
    // built from the exact powers-of-two anchors via successive halving comparisons.
    // We bound it loosely (the measured worst case is ~1.9e-4, well under 4e-3).
    for (int i = 0; i < 64; ++i) {
        const float frac = static_cast<float>(i) / 64.0f; // [0,1)
        const float x = 1.0f + frac;                      // [1,2)
        const float got = gm::Log2Approx(x);
        EXPECT_TRUE(IsFiniteF(got));
        EXPECT_GE(got, -4.0e-3f);       // log2(1)=0 floor
        EXPECT_LE(got, 1.0f + 4.0e-3f); // log2(2)=1 ceil
    }
}

// Non-positive inputs are a defined FINITE floor, never -inf or NaN.
TEST(GameLibsHardening, Log2ApproxNonPositiveIsFinite) {
    EXPECT_TRUE(IsFiniteF(gm::Log2Approx(0.0f)));
    EXPECT_TRUE(IsFiniteF(gm::Log2Approx(-1.0f)));
    EXPECT_TRUE(IsFiniteF(gm::Log2Approx(-1.0e9f)));
    // Subnormal (exp field 0) is treated as the floor too — must stay finite.
    const float subnormal = 1.0e-40f;
    EXPECT_TRUE(IsFiniteF(gm::Log2Approx(subnormal)));
}

// EV monotonicity: stopping DOWN (larger f-number) RAISES EV; a LONGER shutter
// LOWERS it; a HIGHER ISO LOWERS it. These are the producer-side directional claims.
TEST(GameLibsHardening, ExposureValueMonotoneInAperture) {
    gm::LensSettings l;
    l.shutter_s = 0.008f;
    l.iso = 100.0f;
    float prev = -1.0e30f;
    for (float N = 1.0f; N <= 22.0f; N += 0.25f) {
        l.aperture_f = N;
        const float ev = gm::ExposureValue(l);
        EXPECT_TRUE(IsFiniteF(ev));
        EXPECT_GE(ev, prev - 1.0e-4f) << "EV decreased as aperture stopped down at N=" << N;
        prev = ev;
    }
}

TEST(GameLibsHardening, ExposureValueMonotoneInShutterAndIso) {
    gm::LensSettings l;
    l.aperture_f = 2.8f;
    l.iso = 100.0f;
    float prev = 1.0e30f;
    for (float t = 0.0005f; t <= 1.0f; t += 0.01f) {
        l.shutter_s = t;
        const float ev = gm::ExposureValue(l);
        EXPECT_LE(ev, prev + 1.0e-4f) << "EV rose as shutter lengthened at t=" << t;
        prev = ev;
    }
    gm::LensSettings j;
    j.aperture_f = 2.8f;
    j.shutter_s = 0.008f;
    float prevj = 1.0e30f;
    for (float iso = 100.0f; iso <= 6400.0f; iso *= 2.0f) {
        j.iso = iso;
        const float ev = gm::ExposureValue(j);
        EXPECT_LE(ev, prevj + 1.0e-4f) << "EV rose as ISO climbed at iso=" << iso;
        prevj = ev;
    }
}

// ExposureQuality is always clamped [0,1] across messy inputs (negative N, out of
// range luminance), never NaN.
TEST(GameLibsHardening, ExposureQualityBoundedOnMessyInput) {
    for (float N = -3.0f; N <= 30.0f; N += 1.7f) {
        for (float lum = -0.5f; lum <= 1.5f; lum += 0.13f) {
            gm::LensSettings l;
            l.aperture_f = N;
            const float q = gm::ExposureQuality(l, lum);
            ExpectScoreInRange(q, "ExposureQuality");
        }
    }
}

// ComputeDof: aperture->DoF monotonicity (stopping down WIDENS the in-focus band),
// the documented producer-side optical contract.
TEST(GameLibsHardening, ComputeDofBandWidensWithAperture) {
    gm::LensSettings l;
    l.focal_length_mm = 50.0f;
    l.focus_distance_m = 3.0f;
    gm::CameraSubject subj;
    subj.distance_m = 3.0f;
    float prev_band = -1.0f;
    for (float N = 1.4f; N <= 16.0f; N += 0.2f) {
        l.aperture_f = N;
        const gm::DofResult r = gm::ComputeDof(l, subj);
        EXPECT_TRUE(IsFiniteF(r.near_limit_m));
        EXPECT_TRUE(IsFiniteF(r.far_limit_m));
        EXPECT_GE(r.far_limit_m, r.near_limit_m); // far >= near always
        const float band =
            (r.far_limit_m >= gm::kInfiniteFar) ? 1.0e30f : (r.far_limit_m - r.near_limit_m);
        EXPECT_GE(band, prev_band - 1.0e-3f) << "DoF band narrowed as N grew at N=" << N;
        prev_band = band;
    }
}

// A subject sitting exactly at the focus distance is always in focus.
TEST(GameLibsHardening, ComputeDofSubjectAtFocusIsInFocus) {
    gm::LensSettings l;
    l.focal_length_mm = 50.0f;
    l.aperture_f = 2.8f;
    l.focus_distance_m = 3.0f;
    gm::CameraSubject subj;
    subj.distance_m = 3.0f;
    EXPECT_EQ(gm::ComputeDof(l, subj).in_focus, 1.0f);
}

// Degenerate lens (zero/negative focal/aperture/focus distance) must NOT divide by
// zero — every field stays finite, far >= near, in_focus is 0 or 1.
TEST(GameLibsHardening, ComputeDofDegenerateInputsFinite) {
    for (float f = -5.0f; f <= 5.0f; f += 1.1f) {
        for (float N = -3.0f; N <= 5.0f; N += 1.3f) {
            for (float s = -2.0f; s <= 5.0f; s += 1.7f) {
                gm::LensSettings l;
                l.focal_length_mm = f;
                l.aperture_f = N;
                l.focus_distance_m = s;
                gm::CameraSubject subj;
                subj.distance_m = 3.0f;
                const gm::DofResult r = gm::ComputeDof(l, subj);
                EXPECT_TRUE(IsFiniteF(r.near_limit_m)) << "f=" << f << " N=" << N << " s=" << s;
                EXPECT_TRUE(IsFiniteF(r.far_limit_m));
                EXPECT_GE(r.far_limit_m, r.near_limit_m - 1.0e-3f);
                EXPECT_TRUE(r.in_focus == 0.0f || r.in_focus == 1.0f);
            }
        }
    }
}

// Focused at/beyond hyperfocal -> far limit is the finite sentinel (not inf), and
// SubjectIsolation treats that deep DoF as zero depth term.
TEST(GameLibsHardening, ComputeDofHyperfocalSentinelAndIsolationDeep) {
    gm::LensSettings l;
    l.focal_length_mm = 50.0f;
    l.aperture_f = 22.0f;
    l.focus_distance_m = 1000.0f;
    gm::CameraSubject subj;
    subj.distance_m = 1000.0f;
    const gm::DofResult r = gm::ComputeDof(l, subj);
    EXPECT_EQ(r.far_limit_m, gm::kInfiniteFar);
    EXPECT_TRUE(IsFiniteF(r.far_limit_m));
    // A deep-DoF deep-aperture shot is poor isolation.
    EXPECT_LT(gm::SubjectIsolation(l, subj), 0.25f);
}

// SubjectIsolation: a wide-aperture in-focus portrait must isolate BETTER than a
// stopped-down deep one (monotone in aperture), and stay clamped [0,1].
TEST(GameLibsHardening, SubjectIsolationMonotoneInAperture) {
    gm::LensSettings l;
    l.focal_length_mm = 85.0f;
    l.focus_distance_m = 3.0f;
    gm::CameraSubject subj;
    subj.distance_m = 3.0f;
    float prev = 1.0e30f;
    for (float N = 1.0f; N <= 8.0f; N += 0.2f) {
        l.aperture_f = N;
        const float iso = gm::SubjectIsolation(l, subj);
        ExpectScoreInRange(iso, "SubjectIsolation");
        EXPECT_LE(iso, prev + 1.0e-3f) << "isolation rose as aperture stopped down at N=" << N;
        prev = iso;
    }
}

// A subject OUTSIDE the DoF band collapses isolation to its small residual (the
// focus gate). A missed-focus wide-open shot must rank below an in-focus one.
TEST(GameLibsHardening, SubjectIsolationFocusGate) {
    gm::LensSettings l;
    l.focal_length_mm = 85.0f;
    l.aperture_f = 1.8f;
    l.focus_distance_m = 3.0f;
    gm::CameraSubject in_band;
    in_band.distance_m = 3.0f; // at focus
    gm::CameraSubject out_band;
    out_band.distance_m = 50.0f; // far out of focus
    EXPECT_GT(gm::SubjectIsolation(l, in_band), gm::SubjectIsolation(l, out_band));
}

// =====================================================================================
//  PhotoCodex.h  (luminumbra::game)
// =====================================================================================

// Empty codex is the defined zero state.
TEST(GameLibsHardening, PhotoCodexEmptyState) {
    gm::PhotoCodex c;
    EXPECT_EQ(c.species_count(), 0u);
    EXPECT_EQ(c.total_score(), 0.0f);
    EXPECT_EQ(c.completeness(10), 0.0f);
    EXPECT_TRUE(c.entries().empty());
    EXPECT_FALSE(c.discovered(1));
}

// Best-keeping: a later WORSE shot never lowers the record; captures still increment.
TEST(GameLibsHardening, PhotoCodexKeepsBestScore) {
    gm::PhotoCodex c;
    c.Record(7, 0.5f);
    c.Record(7, 0.3f); // worse: must not lower
    c.Record(7, 0.9f); // better: must raise
    c.Record(7, 0.8f); // worse again: must not lower
    ASSERT_EQ(c.species_count(), 1u);
    EXPECT_EQ(c.entries()[0].best_score, 0.9f);
    EXPECT_EQ(c.entries()[0].captures, 4u);
}

// Sorted determinism + order independence: recording in any order yields an
// identical, species_id-sorted entries vector and identical aggregates.
TEST(GameLibsHardening, PhotoCodexSortedOrderIndependent) {
    gm::PhotoCodex c1;
    c1.Record(5, 0.4f);
    c1.Record(1, 0.9f);
    c1.Record(3, 0.2f);
    c1.Record(1, 0.95f);
    gm::PhotoCodex c2;
    c2.Record(1, 0.95f);
    c2.Record(3, 0.2f);
    c2.Record(1, 0.9f);
    c2.Record(5, 0.4f);
    ASSERT_EQ(c1.entries().size(), c2.entries().size());
    for (std::size_t i = 0; i < c1.entries().size(); ++i) {
        EXPECT_EQ(c1.entries()[i].species_id, c2.entries()[i].species_id);
        EXPECT_EQ(c1.entries()[i].best_score, c2.entries()[i].best_score);
        EXPECT_EQ(c1.entries()[i].captures, c2.entries()[i].captures);
        if (i > 0) {
            EXPECT_LT(c1.entries()[i - 1].species_id, c1.entries()[i].species_id);
        }
    }
    EXPECT_EQ(c1.total_score(), c2.total_score());
}

// completeness is clamped [0,1]; an undefined/zero/negative denominator is 0, and
// discovering MORE than the world total saturates at 1.0 (no overflow above 1).
TEST(GameLibsHardening, PhotoCodexCompletenessClamped) {
    gm::PhotoCodex c;
    c.Record(1, 0.5f);
    c.Record(2, 0.5f);
    c.Record(3, 0.5f);
    EXPECT_EQ(c.completeness(0), 0.0f);
    EXPECT_EQ(c.completeness(-5), 0.0f);
    EXPECT_FLOAT_EQ(c.completeness(6), 0.5f);
    EXPECT_EQ(c.completeness(2), 1.0f); // discovered 3 of "2" -> clamp to 1
    EXPECT_GE(c.completeness(100), 0.0f);
    EXPECT_LE(c.completeness(100), 1.0f);
}

// =====================================================================================
//  SpeciesCodex.h  (luminumbra::game)
// =====================================================================================

// Empty registry is the defined zero state; unknown species weight is 0 (NOT the
// baseline 1.0) so callers can distinguish "uncatalogued" from "common".
TEST(GameLibsHardening, SpeciesRegistryEmptyAndUnknownWeightZero) {
    gm::SpeciesRegistry r;
    EXPECT_EQ(r.size(), 0u);
    EXPECT_TRUE(r.all().empty());
    EXPECT_EQ(r.Find(1), nullptr);
    EXPECT_EQ(r.RarityWeight(1), 0.0f);

    gm::SpeciesRegistry d = gm::MakeDefaultSpeciesRegistry();
    EXPECT_EQ(d.RarityWeight(99999), 0.0f); // uncatalogued -> 0
}

// RarityWeight is monotonic in rarity (rarer -> strictly higher weight), and the
// default catalogue iterates species_id-sorted regardless of registration order.
TEST(GameLibsHardening, SpeciesRegistryRarityMonotoneAndSorted) {
    gm::SpeciesRegistry r;
    for (int rarity = 0; rarity <= 5; ++rarity) {
        gm::SpeciesInfo info;
        info.species_id = rarity + 1;
        info.rarity = static_cast<std::uint8_t>(rarity);
        r.Register(info);
    }
    float prev = -1.0f;
    for (int rarity = 0; rarity <= 5; ++rarity) {
        const float w = r.RarityWeight(rarity + 1);
        EXPECT_GT(w, prev) << "rarity weight not strictly increasing at rarity " << rarity;
        prev = w;
    }
    // common (rarity 0) is exactly the baseline 1.0.
    EXPECT_FLOAT_EQ(r.RarityWeight(1), 1.0f);

    gm::SpeciesRegistry d = gm::MakeDefaultSpeciesRegistry();
    const auto& all = d.all();
    ASSERT_FALSE(all.empty());
    for (std::size_t i = 1; i < all.size(); ++i) {
        EXPECT_LT(all[i - 1].species_id, all[i].species_id);
    }
}

// Re-registering an existing id UPDATES in place (no duplicate); size counts distinct
// ids and order-independence holds.
TEST(GameLibsHardening, SpeciesRegistryReRegisterUpdatesInPlace) {
    gm::SpeciesRegistry r;
    r.Register(gm::SpeciesInfo{3, "a", 2, 0, false});
    r.Register(gm::SpeciesInfo{3, "b", 5, 1, true}); // same id -> update
    EXPECT_EQ(r.size(), 1u);
    ASSERT_NE(r.Find(3), nullptr);
    EXPECT_EQ(static_cast<int>(r.Find(3)->rarity), 5);

    gm::SpeciesRegistry r1;
    r1.Register(gm::SpeciesInfo{3, "a", 2, 0, false});
    r1.Register(gm::SpeciesInfo{1, "b", 0, 0, false});
    gm::SpeciesRegistry r2;
    r2.Register(gm::SpeciesInfo{1, "b", 0, 0, false});
    r2.Register(gm::SpeciesInfo{3, "a", 2, 0, false});
    ASSERT_EQ(r1.all().size(), r2.all().size());
    for (std::size_t i = 0; i < r1.all().size(); ++i) {
        EXPECT_EQ(r1.all()[i].species_id, r2.all()[i].species_id);
    }
}

// =====================================================================================
//  LightTools.h  (luminumbra::game)
// =====================================================================================

// Every LightScore axis + total stays clamped [0,1] across a full sweep of (possibly
// out-of-range) scene scalars, never NaN.
TEST(GameLibsHardening, ScoreLightBoundedAcrossSweep) {
    const float vals[] = {-0.5f, 0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 1.5f};
    for (float elev : vals)
        for (float facing : vals)
            for (float cloud : vals)
                for (float ambient : vals) {
                    gm::LightScene s;
                    s.sun_elevation01 = elev;
                    s.subject_facing01 = facing;
                    s.cloud_cover01 = cloud;
                    s.ambient01 = ambient;
                    const gm::LightScore r = gm::ScoreLight(s);
                    ExpectScoreInRange(r.golden_hour, "golden_hour");
                    ExpectScoreInRange(r.rim_light, "rim_light");
                    ExpectScoreInRange(r.softness, "softness");
                    ExpectScoreInRange(r.contrast, "contrast");
                    ExpectScoreInRange(r.total, "light total");
                }
}

// Golden hour is monotonically NON-INCREASING as the sun climbs from horizon to noon
// (the documented "low sun = golden" claim) — the cosine falloff must not wobble.
TEST(GameLibsHardening, GoldenHourMonotoneInElevation) {
    float prev = 1.0e30f;
    for (float e = 0.0f; e <= 0.5f; e += 0.01f) {
        gm::LightScene s;
        s.sun_elevation01 = e;
        s.cloud_cover01 = 0.0f;
        const float g = gm::ScoreGoldenHour(s);
        EXPECT_LE(g, prev + 1.0e-4f) << "golden rose as sun climbed at elev=" << e;
        prev = g;
    }
}

// CosFalloff01 endpoints are (approximately, due to the DM::Cos polynomial) 1 at t=0
// and 0 at t=1, decreasing in between. Tolerance band, NOT exact equality.
TEST(GameLibsHardening, CosFalloff01EndpointsApprox) {
    EXPECT_NEAR(gm::CosFalloff01(0.0f), 1.0f, 1.0e-3f);
    EXPECT_NEAR(gm::CosFalloff01(1.0f), 0.0f, 1.0e-3f);
    float prev = 1.0e30f;
    for (float t = 0.0f; t <= 1.0f; t += 0.05f) {
        const float r = gm::CosFalloff01(t);
        ExpectScoreInRange(r, "CosFalloff01");
        EXPECT_LE(r, prev + 2.0e-3f); // non-increasing (small approx slack)
        prev = r;
    }
}

// Rim light requires BOTH back-lighting and a low sun: a high (overhead) sun yields
// no rim however the subject faces, and the result is clamped.
TEST(GameLibsHardening, RimLightGatedByLowSun) {
    gm::LightScene low;
    low.subject_facing01 = 1.0f;
    low.sun_elevation01 = 0.0f;
    gm::LightScene high;
    high.subject_facing01 = 1.0f;
    high.sun_elevation01 = 1.0f;
    EXPECT_GT(gm::ScoreRimLight(low), gm::ScoreRimLight(high));
    EXPECT_NEAR(gm::ScoreRimLight(high), 0.0f, 1.0e-3f);
}

// run==replay for ScoreLight (pure).
TEST(GameLibsHardening, ScoreLightRunEqualsReplay) {
    gm::LightScene s;
    s.sun_elevation01 = 0.12f;
    s.subject_facing01 = 0.8f;
    s.cloud_cover01 = 0.3f;
    s.ambient01 = 0.25f;
    const gm::LightScore a = gm::ScoreLight(s);
    const gm::LightScore b = gm::ScoreLight(s);
    EXPECT_EQ(a.golden_hour, b.golden_hour);
    EXPECT_EQ(a.rim_light, b.rim_light);
    EXPECT_EQ(a.softness, b.softness);
    EXPECT_EQ(a.contrast, b.contrast);
    EXPECT_EQ(a.total, b.total);
}

// =====================================================================================
//  PhotoSession.h  (luminumbra::game)
// =====================================================================================

// StarsForTotal is a monotonic NON-DECREASING step function: sweeping total upward,
// stars never drop; the documented thresholds fire at exactly their boundaries.
TEST(GameLibsHardening, StarsForTotalMonotoneAndThresholds) {
    int prev = -1;
    for (float t = -0.5f; t <= 1.5f; t += 0.001f) {
        const int s = gm::StarsForTotal(t);
        EXPECT_GE(s, prev) << "stars dropped as total rose at t=" << t;
        EXPECT_GE(s, 0);
        EXPECT_LE(s, 5);
        prev = s;
    }
    // Exact boundary behaviour: at-threshold awards the higher star (>= comparisons).
    EXPECT_EQ(gm::StarsForTotal(0.19f), 0);
    EXPECT_EQ(gm::StarsForTotal(gm::kStar1), 1);
    EXPECT_EQ(gm::StarsForTotal(gm::kStar2), 2);
    EXPECT_EQ(gm::StarsForTotal(gm::kStar3), 3);
    EXPECT_EQ(gm::StarsForTotal(gm::kStar4), 4);
    EXPECT_EQ(gm::StarsForTotal(gm::kStar5), 5);
    EXPECT_EQ(gm::StarsForTotal(1.0f), 5);
    EXPECT_EQ(gm::StarsForTotal(-5.0f), 0); // clamp floor
}

// PRODUCER->CONSUMER contract: EvaluateShot's composition axis must read the rubric's
// composition EXACTLY (it rides directly on it, per the docstring) — not some other
// axis, not a re-scaled value.
TEST(GameLibsHardening, EvaluateShotCompositionMirrorsRubric) {
    gm::ShotInput in;
    in.composition.exposure = 0.5f;
    in.composition.focus = 1.0f;
    in.composition.subjects = {MakeSubject(0.33333334f, 0.33333334f, 0.6f, 0.7f, 1)};
    in.scene_luminance = 0.5f;
    in.main_species_id = 1;
    in.main_subject_distance_m = 3.0f;
    in.main_subject_size_m = 0.6f;
    in.lens.focal_length_mm = 85.0f;
    in.lens.aperture_f = 1.8f;
    in.lens.focus_distance_m = 3.0f;
    in.lens.iso = 100.0f;
    in.lens.shutter_s = 0.004f;

    const ph::PhotoScore rubric = ph::ScorePhoto(in.composition);
    const gm::ShotVerdict v = gm::EvaluateShot(in);
    EXPECT_EQ(v.composition, rubric.composition);
    ExpectScoreInRange(v.exposure, "verdict exposure");
    ExpectScoreInRange(v.focus_isolation, "verdict focus_isolation");
    ExpectScoreInRange(v.total, "verdict total");
    EXPECT_GE(v.stars, 0);
    EXPECT_LE(v.stars, 5);
}

// run==replay for EvaluateShot (pure flow over deterministic libraries).
TEST(GameLibsHardening, EvaluateShotRunEqualsReplay) {
    gm::ShotInput in;
    in.composition.exposure = 0.47f;
    in.composition.focus = 0.82f;
    in.composition.subjects = {MakeSubject(0.31f, -0.30f, 0.55f, 0.68f, 3),
                               MakeSubject(-0.2f, 0.4f, 0.2f, 0.5f, 5)};
    in.scene_luminance = 0.55f;
    in.main_species_id = 3;
    in.main_subject_distance_m = 2.5f;
    in.main_subject_size_m = 0.5f;
    in.lens.focal_length_mm = 50.0f;
    in.lens.aperture_f = 2.0f;
    in.lens.focus_distance_m = 2.5f;
    in.lens.iso = 200.0f;
    in.lens.shutter_s = 0.006f;

    const gm::ShotVerdict a = gm::EvaluateShot(in);
    const gm::ShotVerdict b = gm::EvaluateShot(in);
    EXPECT_EQ(a.composition, b.composition);
    EXPECT_EQ(a.exposure, b.exposure);
    EXPECT_EQ(a.focus_isolation, b.focus_isolation);
    EXPECT_EQ(a.total, b.total);
    EXPECT_EQ(a.stars, b.stars);
}

// CommitShot logs the verdict's TOTAL against the main species and keeps the best
// across repeated commits (a later worse verdict never lowers the codex record),
// while still counting each commit as a capture.
TEST(GameLibsHardening, CommitShotRecordsBestAndCounts) {
    gm::PhotoCodex codex;
    gm::ShotInput in;
    in.main_species_id = 42;
    gm::ShotVerdict good;
    good.total = 0.8f;
    gm::ShotVerdict worse;
    worse.total = 0.3f;
    gm::CommitShot(codex, in, good);
    gm::CommitShot(codex, in, worse); // worse: must NOT lower the record
    ASSERT_EQ(codex.species_count(), 1u);
    EXPECT_TRUE(codex.discovered(42));
    EXPECT_EQ(codex.entries()[0].best_score, 0.8f);
    EXPECT_EQ(codex.entries()[0].captures, 2u);
}

// EvaluateShot's documented contract (PhotoSession.h header) says "An empty /
// subject-less shot is a defined LOW verdict". This regression test ensures
// subject-independent optical terms cannot make an empty frame earn a star.
TEST(GameLibsHardening, EvaluateShotEmptyFrameIsZeroVerdict) {
    gm::ShotInput in; // no subjects in in.composition; default lens/scene
    const gm::ShotVerdict v = gm::EvaluateShot(in);
    EXPECT_EQ(v.composition, 0.0f);
    // An empty frame is a missed shot: zero stars and a near-zero total.
    EXPECT_EQ(v.stars, 0) << "empty (subject-less) frame earned stars: total=" << v.total;
    EXPECT_LT(v.total, gm::kStar1)
        << "empty frame total reached the 1-star threshold via subject-independent "
           "optics: "
        << v.total;
}

} // namespace
